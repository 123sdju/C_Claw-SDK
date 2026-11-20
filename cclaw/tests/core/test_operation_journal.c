#include "cc/app/cc_operation_journal.h"
#include "cc/core/cc_id.h"
#include "../../core/vendor/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 失败即打印行号与表达式并退出当前测试函数，保留当时现场。 */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

/**
 * @brief 比较结果码与预期值，失败时打印诊断并释放结果消息。
 * @param[in] rc 按值移交的结果，调用结束后消息资源已释放。
 * @param[in] expected 本次断言预期的错误码。
 * @param[in] line 调用表达式的源码行号，仅用于诊断。
 * @return 错误码符合预期为一，否则为零。
 */
static int check_result(cc_result_t rc, cc_error_code_t expected, int line)
{
    int ok = rc.code == expected;
    if (!ok) fprintf(stderr, "line %d: got %s: %s\n", line,
        cc_error_code_name(rc.code), rc.message ? rc.message : "");
    cc_result_free(&rc);
    return ok;
}
#define OK(expression) CHECK(check_result((expression), CC_OK, __LINE__))
#define ERROR(expression, code) CHECK(check_result((expression), (code), __LINE__))

/** 顺序测试使用的栈上故障上下文，包装文件系统和日志调用期间保持存活。 */
typedef struct fault_fs {
    cc_filesystem_t *base;          /**< 借用的真实文件系统。 */
    int fail_file_sync;             /**< 持续注入文件同步错误，调用方显式复位。 */
    int fail_dir_sync;              /**< 持续注入目录同步错误，调用方显式复位。 */
    int fail_before_replace;        /**< 在替换真正执行前持续报错。 */
    int fail_after_replace;         /**< 底层替换成功后改报错误，模拟安装结果未知。 */
    int fail_list;                  /**< 持续注入目录列举失败。 */
    int file_sync_calls;            /**< 文件同步回调调用次数，包含故障调用。 */
    int dir_sync_calls;             /**< 目录同步回调调用次数，包含故障调用。 */
    int collide_temporary;          /**< 下一次候选 .tmp 的 stat 前注入既有文件，触发后清零。 */
    int collide_record;             /**< 下一次候选 .json 的 stat 前注入正式与临时文件，触发后清零。 */
    char collision_temporary[512];  /**< 上次注入临时路径，供断言及清理使用。 */
    char collision_record[512];     /**< 上次注入正式路径，供断言及清理使用。 */
} fault_fs_t;

/**
 * @brief 原样转交日志文件文本读取。
 * @param[in] self 必填的借用故障上下文及有效 base。
 * @param[in] path 同步借用的文件路径。
 * @param[out] text 必填输出槽，初始化及失败状态沿用底层契约，成功文本由调用者 free。
 * @return 底层 read_text 结果，消息由调用者释放。
 */
static cc_result_t f_read(void *self, const char *path, char **text)
{ fault_fs_t *f = self; return f->base->vtable->read_text(f->base->self, path, text); }
/**
 * @brief 原样转交临时日志文本写入。
 * @param[in,out] self 必填的借用上下文，操作改变底层文件系统。
 * @param[in] path 同步借用的目标路径。
 * @param[in] text 同步借用的 NUL 结尾文本。
 * @return 底层 write_text 结果，消息由调用者释放。
 */
static cc_result_t f_write(void *self, const char *path, const char *text)
{ fault_fs_t *f = self; return f->base->vtable->write_text(f->base->self, path, text); }
/**
 * @brief 注入目录读取失败或转交真实目录列举。
 * @param[in] self 必填的借用上下文，读取 fail_list。
 * @param[in] path 同步借用的目录路径。
 * @param[out] items 必填数组输出槽，成功数组与各名称由调用者释放，故障分支保持原值。
 * @param[out] count 必填条数输出槽，故障分支保持原值，其余沿用底层契约。
 * @return fail_list 非零为 CC_ERR_IO，其余为底层结果，消息由调用者释放。
 */
static cc_result_t f_list(void *self, const char *path, char ***items, size_t *count)
{
    fault_fs_t *f = self;
    if (f->fail_list) return cc_result_error(CC_ERR_IO, "injected directory read failure");
    return f->base->vtable->list_dir(f->base->self, path, items, count);
}
/**
 * @brief 将日志文件或测试目录删除转交底层。
 * @param[in,out] self 必填的借用上下文，操作改变真实文件系统。
 * @param[in] path 同步借用的待删除路径。
 * @return 底层 remove 结果，消息由调用者释放。
 */
static cc_result_t f_remove(void *self, const char *path)
{ fault_fs_t *f = self; return f->base->vtable->remove(f->base->self, path); }
/**
 * @brief 在候选路径首次 stat 前注入既有文件，再查询真实状态。
 * @param[in,out] self 必填的借用上下文，先清零触发标志并记录路径，注入文件由测试函数清理。
 * @param[in] path 同步借用的路径，通过 /op- 及后缀识别候选，本测试路径短于记录缓冲容量。
 * @param[in,out] stat 必填结构，调用方设置 size，底层输出文件类型、字节数与修改时间。
 * @return 注入写失败时返回该错误，可能已创建部分文件，其余返回底层 stat 结果，消息由调用者释放。
 */
static cc_result_t f_stat(void *self, const char *path, cc_filesystem_stat_t *stat)
{
    fault_fs_t *f = self;
    size_t length = strlen(path);
    int candidate = strstr(path, "/op-") != NULL;
    if (candidate && f->collide_record && length >= 5U && strcmp(path + length - 5U, ".json") == 0) {
        f->collide_record = 0;
        snprintf(f->collision_record, sizeof(f->collision_record), "%s", path);
        snprintf(f->collision_temporary, sizeof(f->collision_temporary), "%.*s.tmp", (int)(length - 5U), path);
        cc_result_t rc = f->base->vtable->write_text(f->base->self, path, "existing record");
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
        rc = f->base->vtable->write_text(f->base->self, f->collision_temporary, "existing temporary");
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }
    if (candidate && f->collide_temporary && length >= 4U && strcmp(path + length - 4U, ".tmp") == 0) {
        f->collide_temporary = 0;
        snprintf(f->collision_temporary, sizeof(f->collision_temporary), "%s", path);
        cc_result_t rc = f->base->vtable->write_text(f->base->self, path, "existing temporary");
        if (rc.code != CC_OK) return rc;
        cc_result_free(&rc);
    }
    return f->base->vtable->stat(f->base->self, path, stat);
}
/**
 * @brief 在真实原子替换之前或成功之后注入错误，模拟两种安装失败现场。
 * @param[in,out] self 必填的借用上下文，读取故障标志并可能改变真实文件。
 * @param[in] from 同步借用的临时日志路径。
 * @param[in] to 同步借用的正式日志路径。
 * @return fail_before_replace 为真时直接 CC_ERR_IO，底层成功且 fail_after_replace 为真时改报 CC_ERR_IO，其余原样返回底层结果，消息由调用者释放。
 */
static cc_result_t f_replace(void *self, const char *from, const char *to)
{
    fault_fs_t *f = self;
    if (f->fail_before_replace) return cc_result_error(CC_ERR_IO, "injected pre-replacement failure");
    cc_result_t rc = f->base->vtable->atomic_replace(f->base->self, from, to);
    if (rc.code == CC_OK && f->fail_after_replace) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO, "injected post-replacement failure");
    }
    return rc;
}
/**
 * @brief 统计每次文件同步调用，并按标志注入失败。
 * @param[in,out] self 必填的借用上下文，先递增 file_sync_calls。
 * @param[in] path 同步借用的文件路径。
 * @return fail_file_sync 为真时 CC_ERR_IO，其余返回底层结果，消息由调用者释放。
 */
static cc_result_t f_sync_file(void *self, const char *path)
{
    fault_fs_t *f = self;
    f->file_sync_calls++;
    if (f->fail_file_sync) return cc_result_error(CC_ERR_IO, "injected file sync failure");
    return f->base->vtable->sync_file(f->base->self, path);
}
/**
 * @brief 统计每次目录同步调用，并按标志注入失败。
 * @param[in,out] self 必填的借用上下文，先递增 dir_sync_calls。
 * @param[in] path 同步借用的目录路径。
 * @return fail_dir_sync 为真时 CC_ERR_IO，其余返回底层结果，消息由调用者释放。
 */
static cc_result_t f_sync_dir(void *self, const char *path)
{
    fault_fs_t *f = self;
    f->dir_sync_calls++;
    if (f->fail_dir_sync) return cc_result_error(CC_ERR_IO, "injected directory sync failure");
    return f->base->vtable->sync_dir(f->base->self, path);
}

/**
 * @brief 查询同名 UNKNOWN 阻断状态并释放查询结果消息。
 * @param[in] journal 借用的日志。
 * @param[in] name 同步借用的工具名称。
 * @return 查询成功返回零或一，查询错误返回负一，供断言区分查询失败。
 */
static int blocked(cc_operation_journal_t *journal, const char *name)
{
    int result = 0;
    cc_result_t rc = cc_operation_journal_is_blocked(journal, name, &result);
    if (rc.code != CC_OK) result = -1;
    cc_result_free(&rc);
    return result;
}

/**
 * @brief 篡改持久化记录验证重复字段、解码 NUL 和重复会话/调用身份均阻止打开。
 *
 * 同时确认字面反斜线 u0000 可正常重开，以及两份单独有效但身份重复的文件均保留。
 * @param[in,out] fs 借用的真实文件系统，创建、改写并删除测试文件及子目录。
 * @param[in] root 同步借用的已存在测试目录，调用期间由本测试独占。
 * @return 全部断言通过并清理成功返回零，CHECK 失败返回一并保留当时资源及文件。
 */
static int check_ambiguous_records(cc_filesystem_t *fs,const char *root)
{
    cc_operation_journal_t *journal=NULL;
    cc_tool_call_t call={"audit-call","write","{\"value\":1}"};
    char *operation=NULL;
    OK(cc_operation_journal_open(fs,root,0,&journal));
    OK(cc_operation_journal_begin(journal,"audit-session",&call,&operation));
    cc_operation_journal_destroy(journal); journal=NULL;
    char path[256]; snprintf(path,sizeof(path),"%s/%s.json",root,operation);
    char *original=NULL; OK(fs->vtable->read_text(fs->self,path,&original)); CHECK(original);
    size_t length=strlen(original); CHECK(length && original[length-1]=='}');
    const char duplicate[] = ",\"tool_name\":\"other\"}";
    char *changed=malloc(length+sizeof(duplicate)); CHECK(changed);
    memcpy(changed,original,length-1); memcpy(changed+length-1,duplicate,sizeof(duplicate));
    OK(fs->vtable->write_text(fs->self,path,changed));
    ERROR(cc_operation_journal_open(fs,root,0,&journal),CC_ERR_STORAGE); CHECK(!journal);
    char *retained=NULL; OK(fs->vtable->read_text(fs->self,path,&retained));
    CHECK(retained && strcmp(retained,changed)==0); free(retained); free(changed);

    const char *needle="\"tool_name\":\"write\"";
    const char *match=strstr(original,needle); CHECK(match);
    size_t prefix=(size_t)(match-original)+strlen(needle)-1U;
    const char injected[]="\\u0000suffix";
    changed=malloc(length+sizeof(injected)); CHECK(changed);
    memcpy(changed,original,prefix); memcpy(changed+prefix,injected,sizeof(injected)-1U);
    memcpy(changed+prefix+sizeof(injected)-1U,original+prefix,length-prefix+1U);
    OK(fs->vtable->write_text(fs->self,path,changed));
    ERROR(cc_operation_journal_open(fs,root,0,&journal),CC_ERR_STORAGE); CHECK(!journal);
    OK(fs->vtable->write_text(fs->self,path,original)); free(changed); free(original);
    OK(cc_operation_journal_open(fs,root,0,&journal)); CHECK(blocked(journal,"write")==1);
    OK(cc_operation_journal_finish(journal,operation));
    cc_operation_journal_destroy(journal); journal=NULL; free(operation);

    /* 已转义的字面反斜线 u 序列继续按普通字符串保存。 */
    cc_tool_call_t literal={"literal","write","{\"text\":\"\\\\u0000\"}"};
    OK(cc_operation_journal_open(fs,root,0,&journal));
    OK(cc_operation_journal_begin(journal,"audit-session",&literal,&operation));
    cc_operation_journal_destroy(journal); journal=NULL;
    OK(cc_operation_journal_open(fs,root,0,&journal)); CHECK(blocked(journal,"write")==1);
    OK(cc_operation_journal_finish(journal,operation));
    cc_operation_journal_destroy(journal); journal=NULL; free(operation);

    /* 两份有效文件可共享同一会话/调用身份，打开须报错并保留两份原文件。 */
    char a[256],b[256],a_first[330],a_second[330],b_second[330];
    snprintf(a,sizeof(a),"%s/duplicate-a",root); snprintf(b,sizeof(b),"%s/duplicate-b",root);
    OK(fs->vtable->make_dir(fs->self,a)); OK(fs->vtable->make_dir(fs->self,b));
    char *first=NULL,*second=NULL;
    OK(cc_operation_journal_open(fs,a,0,&journal));
    OK(cc_operation_journal_begin(journal,"same-session",&call,&first));
    cc_operation_journal_destroy(journal); journal=NULL;
    OK(cc_operation_journal_open(fs,b,0,&journal));
    OK(cc_operation_journal_begin(journal,"same-session",&call,&second));
    cc_operation_journal_destroy(journal); journal=NULL;
    snprintf(a_first,sizeof(a_first),"%s/%s.json",a,first);
    snprintf(a_second,sizeof(a_second),"%s/%s.json",a,second);
    snprintf(b_second,sizeof(b_second),"%s/%s.json",b,second);
    char *second_text=NULL; OK(fs->vtable->read_text(fs->self,b_second,&second_text)); CHECK(second_text);
    OK(fs->vtable->write_text(fs->self,a_second,second_text)); free(second_text);
    ERROR(cc_operation_journal_open(fs,a,0,&journal),CC_ERR_STORAGE); CHECK(!journal);
    cc_filesystem_stat_t info={.size=sizeof(info)};
    OK(fs->vtable->stat(fs->self,a_first,&info)); CHECK(info.type==CC_FS_ENTRY_FILE);
    OK(fs->vtable->stat(fs->self,a_second,&info)); CHECK(info.type==CC_FS_ENTRY_FILE);
    OK(fs->vtable->remove(fs->self,a_first)); OK(fs->vtable->remove(fs->self,a_second));
    OK(fs->vtable->remove(fs->self,b_second)); OK(fs->vtable->remove(fs->self,a));
    OK(fs->vtable->remove(fs->self,b)); free(first); free(second);
    return 0;
}

/**
 * @brief 验证不确定安装后的重复导入必须核对磁盘身份并完成同步。
 *
 * 覆盖替换前失败、替换后报错、同步失败、文件损坏/缺失以及恢复有效文件后的同 ID 重试。
 * @param[in,out] fs 借用的真实文件系统，用于直接读取、篡改和恢复正式文件。
 * @param[in,out] wrapped 借用的故障包装文件系统，须绑定 fault 并在日志销毁前存活。
 * @param[in,out] fault 借用的故障上下文，分阶段设置/清除标志并比较同步计数。
 * @param[in] root 同步借用的既有测试目录，本函数独占。
 * @return 全部断言及成功清理完成为零，CHECK 失败为一并保留现场，成功释放日志、ID、列表及文本。
 */
static int check_retry_import(cc_filesystem_t *fs, cc_filesystem_t *wrapped,
                              fault_fs_t *fault, const char *root)
{
    cc_operation_journal_t *journal=NULL;
    cc_tool_call_t call={"import-call","camera","{\"x\":1}"};
    char *operation=NULL;
    OK(cc_operation_journal_open(wrapped,root,0,&journal));
    /* 替换前失败也保留内存 UNKNOWN，重复导入需核对缺失的正式文件。 */
    fault->fail_before_replace=1;
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_IO);
    CHECK(!operation && blocked(journal,"camera")==1);
    fault->fail_before_replace=0;
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_STORAGE);
    CHECK(!operation && blocked(journal,"camera")==1);
    cc_operation_journal_destroy(journal); journal=NULL;
    OK(cc_operation_journal_open(wrapped,root,0,&journal));
    OK(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation));
    CHECK(operation && blocked(journal,"camera")==1);
    OK(cc_operation_journal_finish(journal,operation)); free(operation); operation=NULL;

    /* 安装已完成而响应丢失时，重复导入须校验文件并完成文件及目录同步。 */
    fault->fail_after_replace=1;
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_IO);
    CHECK(!operation && blocked(journal,"camera")==1);
    fault->fail_after_replace=0;
    cc_pending_operation_t *items=NULL; size_t count=0;
    OK(cc_operation_journal_list(journal,&items,&count)); CHECK(count==1);
    char path[256]; snprintf(path,sizeof(path),"%s/%s.json",root,items[0].operation_id);
    char *saved=NULL; OK(fs->vtable->read_text(fs->self,path,&saved)); CHECK(saved);
    fault->fail_file_sync=1;
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_IO);
    CHECK(!operation && blocked(journal,"camera")==1);
    fault->fail_file_sync=0;
    if (fs->capabilities & CC_FS_CAP_DIR_SYNC) {
        fault->fail_dir_sync=1;
        ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_IO);
        CHECK(!operation && blocked(journal,"camera")==1);
        fault->fail_dir_sync=0;
    }
    OK(fs->vtable->write_text(fs->self,path,"{}"));
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_STORAGE);
    CHECK(!operation && blocked(journal,"camera")==1);
    OK(fs->vtable->remove(fs->self,path));
    ERROR(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation),CC_ERR_STORAGE);
    CHECK(!operation && blocked(journal,"camera")==1);
    OK(fs->vtable->write_text(fs->self,path,saved)); free(saved);
    int before_file=fault->file_sync_calls, before_dir=fault->dir_sync_calls;
    OK(cc_operation_journal_import_unknown(journal,"import-session",&call,&operation));
    CHECK(operation && strcmp(operation,items[0].operation_id)==0 &&
          fault->file_sync_calls>before_file &&
          (!(fs->capabilities & CC_FS_CAP_DIR_SYNC) || fault->dir_sync_calls>before_dir));
    cc_operation_journal_destroy(journal); journal=NULL;
    OK(cc_operation_journal_open(wrapped,root,0,&journal));
    CHECK(blocked(journal,"camera")==1);
    OK(cc_operation_journal_finish(journal,operation));
    cc_operation_journal_destroy(journal); free(operation);
    cc_pending_operation_array_free(items,count);
    return 0;
}

/**
 * @brief 注入既有临时路径和正式路径冲突，验证准入失败保留原文件且队列为空。
 * @param[in,out] fs 借用的真实文件系统，直接检查内容并删除注入文件。
 * @param[in,out] wrapped 借用的故障包装文件系统，与 fault 绑定。
 * @param[in,out] fault 借用的上下文，开启一次性冲突标志并读取捕获的随机 ID 路径。
 * @param[in] root 同步借用的既有空测试目录，独占至日志销毁。
 * @return 两种冲突都拒绝准入、输出为空、记录数为零且原内容完整时返回零，断言失败返回一并保留现场。
 */
static int check_existing_paths(cc_filesystem_t *fs, cc_filesystem_t *wrapped,
                                fault_fs_t *fault, const char *root)
{
    cc_operation_journal_t *journal = NULL;
    cc_tool_call_t call = {"collision-call", "write", "{}"};
    char *operation = NULL, *text = NULL;
    cc_pending_operation_t *items = NULL;
    size_t count = 0;
    OK(cc_operation_journal_open(wrapped, root, 0, &journal));
    fault->collide_temporary = 1;
    ERROR(cc_operation_journal_begin(journal, "collision-session", &call, &operation), CC_ERR_STORAGE);
    CHECK(!operation && !fault->collide_temporary && blocked(journal, "write") == 0);
    OK(cc_operation_journal_list(journal, &items, &count)); CHECK(!items && count == 0);
    OK(fs->vtable->read_text(fs->self, fault->collision_temporary, &text));
    CHECK(text && strcmp(text, "existing temporary") == 0); free(text); text = NULL;
    OK(fs->vtable->remove(fs->self, fault->collision_temporary));

    fault->collide_record = 1;
    ERROR(cc_operation_journal_begin(journal, "collision-session", &call, &operation), CC_ERR_STORAGE);
    CHECK(!operation && !fault->collide_record && blocked(journal, "write") == 0);
    OK(cc_operation_journal_list(journal, &items, &count)); CHECK(!items && count == 0);
    OK(fs->vtable->read_text(fs->self, fault->collision_record, &text));
    CHECK(text && strcmp(text, "existing record") == 0); free(text); text = NULL;
    OK(fs->vtable->read_text(fs->self, fault->collision_temporary, &text));
    CHECK(text && strcmp(text, "existing temporary") == 0); free(text);
    OK(fs->vtable->remove(fs->self, fault->collision_record));
    OK(fs->vtable->remove(fs->self, fault->collision_temporary));
    cc_operation_journal_destroy(journal);
    return 0;
}

/**
 * @brief 拒绝 cJSON 的每次内存申请，模拟有效账本解析时的分配失败。
 * @param[in] size 本次请求字节数，故障回调忽略其值。
 * @return 始终返回 NULL，故障钩子仅在单线程解析回归期间安装。
 */
static void *reject_json_allocation(size_t size)
{
    (void)size;
    return NULL;
}

/**
 * @brief 对有效正式记录注入 cJSON 分配失败，验证失败分类保持 OTHER。
 * @param[in,out] fs 借用的真实文件系统，成功清理本次操作文件及日志对象。
 * @param[in] root 同步借用的既有空目录，由当前顺序测试独占。
 * @return 分配故障未被误判为完整性损坏且恢复钩子后可重新打开为零，断言失败为一并保留现场。
 */
static int check_parser_allocation_failure(cc_filesystem_t *fs, const char *root)
{
    cc_operation_journal_t *journal=NULL;
    cc_tool_call_t call={"allocation-call","write","{}"}; char *operation=NULL;
    OK(cc_operation_journal_open(fs,root,0,&journal));
    OK(cc_operation_journal_begin(journal,"allocation-session",&call,&operation));
    cc_operation_journal_destroy(journal); journal=NULL;
    cJSON_Hooks hooks={reject_json_allocation,free};
    cc_operation_journal_failure_t failure=CC_OPERATION_JOURNAL_FAILURE_INTEGRITY;
    cJSON_InitHooks(&hooks);
    cc_result_t rc=cc_operation_journal_open_detailed(fs,root,0,&journal,&failure);
    cJSON_InitHooks(NULL);
    CHECK(check_result(rc,CC_ERR_STORAGE,__LINE__) && !journal &&
          failure==CC_OPERATION_JOURNAL_FAILURE_OTHER);
    OK(cc_operation_journal_open(fs,root,0,&journal));
    OK(cc_operation_journal_finish(journal,operation)); free(operation);
    cc_operation_journal_destroy(journal);
    return 0;
}

/**
 * @brief 顺序运行操作日志身份、准入、重开、损坏及故障恢复回归。
 *
 * 随机目录隔离测试文件，依次覆盖同名 UNKNOWN、阈值、歧义记录、同步/替换故障、导入重试、
 * 路径冲突及有效记录的解析分配失败。成功路径释放全部资源并删除测试目录。
 * @return 全部断言通过返回零，CHECK 失败返回一并保留当时文件和尚未释放资源。
 */
int main(void)
{
    cc_filesystem_t fs = {0};
    OK(cc_filesystem_get_default(&fs));
    char *random_id = NULL;
    OK(cc_id_generate_uuid_v4("journal-test-", &random_id));
    OK(fs.vtable->make_dir(fs.self, random_id));

    cc_operation_journal_t *journal = NULL;
    OK(cc_operation_journal_open(&fs, random_id, 3, &journal));
    cc_tool_call_t action = {"call-1", "write", "{\"value\":1}"};
    cc_tool_call_t same = {"call-2", "write", "{}"};
    cc_tool_call_t other = {"call-3", "read_other", "{}"};
    char *first = NULL, *second = NULL, *third = NULL, *denied = NULL;
    OK(cc_operation_journal_begin(journal, "session-a", &action, &first));
    int found=0;
    OK(cc_operation_journal_contains(journal,"session-a",&action,&found)); CHECK(found);
    cc_tool_call_t conflicting_name={"call-1","read_other","{\"value\":1}"};
    ERROR(cc_operation_journal_contains(journal,"session-a",&conflicting_name,&found),CC_ERR_STORAGE);
    CHECK(!found);
    cc_tool_call_t conflicting_args={"call-1","write","{\"value\":2}"};
    ERROR(cc_operation_journal_contains(journal,"session-a",&conflicting_args,&found),CC_ERR_STORAGE);
    CHECK(!found);
    OK(cc_operation_journal_contains(journal,"other-session",&conflicting_name,&found)); CHECK(!found);
    OK(cc_operation_journal_begin(journal, "session-b", &same, &second));
    CHECK(blocked(journal, "write") == 0);
    OK(cc_operation_journal_mark_unknown(journal, first));
    CHECK(blocked(journal, "write") == 1);
    ERROR(cc_operation_journal_begin(journal, "session-c", &same, &denied), CC_ERR_INVALID_STATE);
    CHECK(denied == NULL);
    OK(cc_operation_journal_begin(journal, "session-c", &other, &third));
    cc_tool_call_t fourth = {"call-4", "different", "{}"};
    ERROR(cc_operation_journal_begin(journal, "session-d", &fourth, &denied), CC_ERR_LIMIT_EXCEEDED);
    cc_operation_journal_destroy(journal);

    /* 使用较小准入阈值重开时，已有三条记录继续保留且均成为 UNKNOWN。 */
    OK(cc_operation_journal_open(&fs, random_id, 1, &journal));
    cc_pending_operation_t *items = NULL;
    size_t count = 0;
    OK(cc_operation_journal_list(journal, &items, &count));
    CHECK(count == 3);
    for (size_t i = 0; i < count; i++) CHECK(items[i].in_progress == 0);
    cc_pending_operation_array_free(items, count);
    OK(cc_operation_journal_finish(journal, first));
    CHECK(blocked(journal, "write") == 1);
    OK(cc_operation_journal_finish(journal, second));
    CHECK(blocked(journal, "write") == 0);
    ERROR(cc_operation_journal_begin(journal, "session-d", &fourth, &denied), CC_ERR_LIMIT_EXCEEDED);
    OK(cc_operation_journal_finish(journal, third));
    cc_operation_journal_destroy(journal);
    free(first); free(second); free(third);
    CHECK(check_ambiguous_records(&fs,random_id)==0);

    /* 派发前同步失败可撤销准入，替换结果未知时保留同名工具阻断。 */
    fault_fs_t fault = {0};
    fault.base = &fs;
    cc_filesystem_vtable_t vtable = {0};
    vtable.size = sizeof(vtable);
    vtable.read_text = f_read;
    vtable.write_text = f_write;
    vtable.list_dir = f_list;
    vtable.remove = f_remove;
    vtable.stat = f_stat;
    vtable.atomic_replace = f_replace;
    vtable.sync_file = f_sync_file;
    vtable.sync_dir = f_sync_dir;
    cc_filesystem_t wrapped = fs;
    wrapped.self = &fault;
    wrapped.vtable = &vtable;
    fault.fail_list = 1;
    ERROR(cc_operation_journal_open(&wrapped, random_id, 0, &journal), CC_ERR_IO);
    CHECK(journal == NULL);
    fault.fail_list = 0;
    OK(cc_operation_journal_open(&wrapped, random_id, 0, &journal));
    fault.fail_file_sync = 1;
    ERROR(cc_operation_journal_begin(journal, "session-a", &action, &denied), CC_ERR_IO);
    CHECK(denied == NULL && blocked(journal, "write") == 0);
    fault.fail_file_sync = 0;
    fault.fail_after_replace = 1;
    ERROR(cc_operation_journal_begin(journal, "session-a", &action, &denied), CC_ERR_IO);
    CHECK(denied == NULL && blocked(journal, "write") == 1);
    items = NULL; count = 0;
    OK(cc_operation_journal_list(journal, &items, &count));
    CHECK(count == 1 && items[0].in_progress == 0);
    fault.fail_after_replace = 0;
    if (fs.capabilities & CC_FS_CAP_DIR_SYNC) {
        fault.fail_dir_sync = 1;
        ERROR(cc_operation_journal_finish(journal, items[0].operation_id), CC_ERR_IO);
        CHECK(blocked(journal, "write") == 1);
        fault.fail_dir_sync = 0;
    }
    OK(cc_operation_journal_finish(journal, items[0].operation_id));
    cc_pending_operation_array_free(items, count);
    cc_operation_journal_destroy(journal);
    CHECK(check_retry_import(&fs,&wrapped,&fault,random_id)==0);
    CHECK(check_existing_paths(&fs,&wrapped,&fault,random_id)==0);
    CHECK(check_parser_allocation_failure(&fs,random_id)==0);

    /* 阈值零采用默认 64 条，达到阈值后拒绝普通新操作。 */
    OK(cc_operation_journal_open(&fs, random_id, 0, &journal));
    char *ids[CC_OPERATION_JOURNAL_DEFAULT_LIMIT] = {0};
    for (size_t i = 0; i < CC_OPERATION_JOURNAL_DEFAULT_LIMIT; i++) {
        char call_id[32];
        snprintf(call_id, sizeof(call_id), "capacity-%zu", i);
        cc_tool_call_t capacity_call = {call_id, "capacity_test", "{}"};
        OK(cc_operation_journal_begin(journal, "capacity-session", &capacity_call, &ids[i]));
    }
    ERROR(cc_operation_journal_begin(journal, "session-d", &fourth, &denied), CC_ERR_LIMIT_EXCEEDED);
    for (size_t i = 0; i < CC_OPERATION_JOURNAL_DEFAULT_LIMIT; i++) {
        OK(cc_operation_journal_finish(journal, ids[i]));
        free(ids[i]);
    }
    cc_operation_journal_destroy(journal);

    /* 正式记录损坏时，重新打开返回存储错误并保持输出为空。 */
    OK(cc_operation_journal_open(&fs, random_id, 0, &journal));
    first = NULL;
    OK(cc_operation_journal_begin(journal, "session-a", &action, &first));
    cc_operation_journal_destroy(journal);
    char path[512];
    CHECK(snprintf(path, sizeof(path), "%s/%s.json", random_id, first) > 0);
    OK(fs.vtable->write_text(fs.self, path, "{\"broken\":true}"));
    journal = NULL;
    ERROR(cc_operation_journal_open(&fs, random_id, 0, &journal), CC_ERR_STORAGE);
    CHECK(journal == NULL);
    OK(fs.vtable->remove(fs.self, path));
    OK(fs.vtable->remove(fs.self, random_id));
    free(first);
    free(random_id);
    fs.vtable->destroy(fs.self);
    return 0;
}

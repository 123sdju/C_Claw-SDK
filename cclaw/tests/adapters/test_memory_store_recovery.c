#include "cc/core/cc_id.h"
#include "cc/adapters/cc_json_memory_store.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_memory_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 检查一次测试表达式，失败时向标准错误输出行号并从当前测试函数返回一。
 * @param[in] x 求值一次的断言表达式，非零表示通过。
 * @note 失败直接退出当前函数，之后的局部清理语句不再执行，进程退出负责回收剩余内存。
 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 将操作结果转换为断言布尔值，输出错误消息后释放结果拥有的消息。
 * @param[in] rc 按值移交的结果，调用后原结果中的拥有消息已释放，调用者须避免再次释放该消息。
 * @return rc.code 为 CC_OK 时返回一，其他状态返回零。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

/** @brief 借用真实文件系统的单线程故障夹具，各开关只影响对应包装回调。 */
typedef struct fault_fs {
    cc_filesystem_t *base; /**< 借用的基础文件系统，生命周期覆盖包装器和存储。 */
    int fail_write; /**< 零转发写入，一直接报错，二先写入部分证据再报错。 */
    int fail_replace_after; /**< 非零时在底层替换及可选备份写入成功后报告错误。 */
    int fail_dir_sync; /**< 非零时直接报告目录同步错误。 */
    int fail_replace_before; /**< 非零时在调用底层替换前报告错误。 */
    int retain_backup; /**< 非零时在底层替换成功后写入目标路径加 .bak 的证据文件。 */
    int fail_remove; /**< 非零时跳过底层删除并报告错误。 */
} fault_fs_t;
/** @brief f_read 的累计调用次数，包含底层失败的读取尝试，用于检查超限文件在回读前被拒绝。 */
static unsigned read_calls;
/**
 * @brief 累加读取次数并转发至基础文件系统。
 * @param[in] s 借用的有效 fault_fs_t 上下文，base 及其 read_text 回调必须存在。
 * @param[in] p 调用期间借用的读取路径。
 * @param[out] v 基础 read_text 的正文输出，初始化及失败语义沿用该回调，成功正文由调用者 free。
 * @return 基础读取结果原样返回，每次调用均增加 read_calls。
 */
static cc_result_t f_read(void *s,const char *p,char **v) { fault_fs_t *f=s; read_calls++; return f->base->vtable->read_text(f->base->self,p,v); }
/**
 * @brief 按 fail_write 注入写入故障，支持先留下部分文件内容再失败。
 * @param[in] s 借用的有效故障上下文，base 的 write_text 必须存在。
 * @param[in] p 调用期间借用的目标路径。
 * @param[in] v 调用期间借用的待写正文，正常分支原样转发，故障分支可能使用固定的部分证据正文。
 * @return 部分证据写入失败时返回底层错误，故障开关非零时返回 CC_ERR_IO，其他情况返回底层写入结果。
 */
static cc_result_t f_write(void *s,const char *p,const char *v) { fault_fs_t *f=s; if (f->fail_write==2) { cc_result_t rc=f->base->vtable->write_text(f->base->self,p,"partial evidence"); if (rc.code!=CC_OK) return rc; } if (f->fail_write) return cc_result_error(CC_ERR_IO,"injected full disk"); return f->base->vtable->write_text(f->base->self,p,v); }
/**
 * @brief 将路径状态检查原样转发给基础文件系统。
 * @param[in] s 借用的有效故障上下文，base 的 stat 必须存在。
 * @param[in] p 调用期间借用的待检查路径。
 * @param[in,out] v 必需的状态结构，调用者初始化 size，其他字段由底层 stat 填写。
 * @return 基础 stat 结果原样返回，输出的错误语义沿用底层实现。
 */
static cc_result_t f_stat(void *s,const char *p,cc_filesystem_stat_t *v) { fault_fs_t *f=s; return f->base->vtable->stat(f->base->self,p,v); }
/**
 * @brief 根据 fail_remove 拒绝删除，或将删除请求转发至基础文件系统。
 * @param[in] s 借用的有效故障上下文，正常分支要求 base 的 remove 存在。
 * @param[in] p 调用期间借用的目标路径。
 * @return 故障开关非零时为 CC_ERR_IO 且跳过底层删除，其他情况返回底层删除结果。
 */
static cc_result_t f_remove(void *s,const char *p) { fault_fs_t *f=s; if (f->fail_remove) return cc_result_error(CC_ERR_IO,"injected cleanup failure"); return f->base->vtable->remove(f->base->self,p); }
/**
 * @brief 在替换前或替换后注入错误，并可在替换成功后留下备份证据。
 * @param[in] s 借用的有效故障上下文，base 提供 atomic_replace 及可选备份使用的 write_text。
 * @param[in] a 调用期间借用的临时源路径，正常替换由底层消费源文件。
 * @param[in] b 调用期间借用的目标路径，retain_backup 时还向该路径加 .bak 写入固定证据。
 * @return 前置或后置故障为 CC_ERR_IO，底层替换和备份写入错误原样返回，全部通过为 CC_OK。
 * @note 后置错误发生时目标已由底层安装，存储必须核对实际可见文件。备份路径使用固定 200 字节缓冲。
 */
static cc_result_t f_replace(void *s,const char *a,const char *b) {
    fault_fs_t *f=s;
    if (f->fail_replace_before) return cc_result_error(CC_ERR_IO,"injected rename failure");
    cc_result_t rc=f->base->vtable->atomic_replace(f->base->self,a,b);
    if (rc.code==CC_OK && f->retain_backup) {
        char backup[200]; snprintf(backup,sizeof(backup),"%s.bak",b);
        rc=f->base->vtable->write_text(f->base->self,backup,"backup evidence");
    }
    if (rc.code==CC_OK && f->fail_replace_after) return cc_result_error(CC_ERR_IO,"injected ambiguous replace");
    return rc;
}
/**
 * @brief 将文件同步请求原样转发给基础文件系统。
 * @param[in] s 借用的有效故障上下文，base 的 sync_file 必须存在。
 * @param[in] p 调用期间借用的文件路径。
 * @return 基础文件同步结果原样返回。
 */
static cc_result_t f_sync(void *s,const char *p) { fault_fs_t *f=s; return f->base->vtable->sync_file(f->base->self,p); }
/**
 * @brief 根据 fail_dir_sync 注入目录同步故障。
 * @param[in] s 借用的有效故障上下文，正常分支要求 base 的 sync_dir 存在。
 * @param[in] p 调用期间借用的目录路径。
 * @return 故障开关非零时为 CC_ERR_IO，其他情况返回基础目录同步结果。
 */
static cc_result_t f_sync_dir(void *s,const char *p) { fault_fs_t *f=s; if (f->fail_dir_sync) return cc_result_error(CC_ERR_IO,"injected directory sync failure"); return f->base->vtable->sync_dir(f->base->self,p); }

/**
 * @brief 按主键读取记录，核对正文或预期缺失状态，并清理临时读取结果。
 * @param[in] store 借用的有效存储。
 * @param[in] key 调用期间借用的待查询主键。
 * @param[in] expected 借用的期望正文，NULL 表示要求返回 CC_ERR_NOT_FOUND，空字符串表示要求存在空正文。
 * @return 状态及正文符合预期为一，其他情况为零，返回前始终释放读取结果及记录副本。
 */
static int entry_is(cc_memory_store_t *store, const char *key, const char *expected)
{
    cc_memory_entry_t entry={0};
    cc_result_t rc=cc_memory_store_get(store,key,&entry);
    int matches=expected ? rc.code==CC_OK && strcmp(entry.value,expected)==0 : rc.code==CC_ERR_NOT_FOUND;
    cc_result_free(&rc); cc_memory_entry_free(&entry); return matches;
}

/**
 * @brief 验证普通记录和受限摘要的空主键写入在修改文件、记录或修订号前被拒绝。
 * @param[in] fs 借用的基础文件系统，用于比较磁盘正文和清理测试文件。
 * @param[in] wrapped 借用的存储文件系统，生命周期覆盖创建和重开。
 * @param[in] path 借用的独立测试文件路径。
 * @return 所有断言通过为零，任一断言失败为一。
 * @note 成功时销毁存储、释放副本并移除 path，断言失败时立即返回，后续清理不再执行。
 */
static int test_empty_keys(cc_filesystem_t *fs, cc_filesystem_t *wrapped, const char *path)
{
    const char *initial = "[{\"key\":\"managed\",\"value\":\"protected\",\"category\":\"fact\",\"managed\":true,\"importance\":30,\"created_at\":100,\"updated_at\":100}]";
    CHECK(ok(fs->vtable->write_text(fs->self, path, initial)));
    cc_memory_store_t store = {0};
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped)));
    CHECK(ok(cc_memory_store_set(&store, "ordinary", "preserved", "fact", NULL)));

    cc_memory_plan_t baseline = {0};
    uint64_t revision = 0;
    CHECK(ok(cc_json_memory_managed_list(&store, &baseline, &revision)));
    CHECK(baseline.count == 1 && strcmp(baseline.records[0].id, "managed") == 0);
    char *original = NULL;
    CHECK(ok(fs->vtable->read_text(fs->self, path, &original)));

    const char *categories[] = {"fact", "context_summary"};
    for (size_t i = 0; i < sizeof(categories) / sizeof(categories[0]); i++) {
        if (i == 1) CHECK(ok(cc_json_memory_set_summary_cache_limits(&store, 1, 1, 64)));
        cc_result_t rc = cc_memory_store_set(&store, "", "invalid", categories[i], "session");
        CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
        cc_result_free(&rc);
        CHECK(entry_is(&store, "ordinary", "preserved"));
        CHECK(entry_is(&store, "managed", "protected") && entry_is(&store, "", NULL));
        int ready = 0;
        CHECK(ok(cc_json_memory_write_ready(&store, &ready)) && ready);

        cc_memory_plan_t current = {0};
        uint64_t current_revision = 0;
        CHECK(ok(cc_json_memory_managed_list(&store, &current, &current_revision)));
        CHECK(current_revision == revision && current.count == baseline.count);
        CHECK(strcmp(current.records[0].id, baseline.records[0].id) == 0);
        CHECK(strcmp(current.records[0].text, baseline.records[0].text) == 0);
        CHECK(current.records[0].updated_at == baseline.records[0].updated_at);
        cc_memory_plan_cleanup(&current);
        char *after = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, path, &after)));
        CHECK(strcmp(original, after) == 0);
        free(after);
    }
    free(original);
    cc_memory_plan_cleanup(&baseline);
    cc_memory_store_destroy(&store);

    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped)));
    CHECK(entry_is(&store, "ordinary", "preserved") && entry_is(&store, "managed", "protected"));
    CHECK(entry_is(&store, "", NULL));
    cc_memory_store_destroy(&store);
    CHECK(ok(fs->vtable->remove(fs->self, path)));
    return 0;
}

/**
 * @brief 验证持久记忆拒绝解码 NUL，保留失败证据，并可无损读写字面反斜杠及正常 Unicode。
 * @param[in] fs 借用的基础文件系统，用于写入、逐字节检查和清理夹具。
 * @param[in] wrapped 借用的存储文件系统，故障开关为零，生命周期覆盖全部存储实例。
 * @param[in] path 借用的独立测试路径，由 main 的 160 字节缓冲提供，旁路名称使用本函数的 200 字节缓冲。
 * @return 所有断言通过为零，失败为一，成功时销毁存储、释放副本并删除主文件和旁路夹具。
 * @note 断言失败直接返回，可能保留当前对象和磁盘夹具供核查。
 */
static int test_nul_escapes(cc_filesystem_t *fs, cc_filesystem_t *wrapped, const char *path)
{
    const char *invalid[] = {
        "[{\"key\":\"a\\u0000tail\",\"value\":\"x\"}]",
        "[{\"key\":\"a\",\"value\":\"x\\u0000tail\"}]",
        "[{\"key\":\"a\",\"value\":\"x\",\"category\":\"cat\\u0000tail\"}]",
        "[{\"key\":\"a\",\"value\":\"x\",\"session_id\":\"s\\u0000tail\"}]",
        "[{\"key\":\"a\",\"value\":\"x\",\"category\":\"fact\",\"managed\":true,\"importance\":30,\"evidence\":\"e\\u0000tail\"}]",
        "[{\"key\\u0000tail\":\"a\",\"value\":\"x\"}]",
        "[{\"key\":\"a\",\"value\\u0000tail\":\"x\"}]"
    };
    const char *backup_text = "preserved backup evidence";
    const char *temporary_text = "preserved temporary evidence";
    char backup[200], temporary[200];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(ok(fs->vtable->write_text(fs->self, path, invalid[i])));
        CHECK(ok(fs->vtable->write_text(fs->self, backup, backup_text)));
        CHECK(ok(fs->vtable->write_text(fs->self, temporary, temporary_text)));
        cc_memory_store_t store = {0};
        cc_result_t rc = cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped);
        CHECK(rc.code == CC_ERR_STORAGE && !store.self && !store.vtable);
        cc_result_free(&rc);
        char *content = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, path, &content)));
        CHECK(strcmp(content, invalid[i]) == 0);
        free(content);
        content = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, backup, &content)));
        CHECK(strcmp(content, backup_text) == 0);
        free(content);
        content = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, temporary, &content)));
        CHECK(strcmp(content, temporary_text) == 0);
        free(content);
        CHECK(ok(fs->vtable->remove(fs->self, backup)));
        CHECK(ok(fs->vtable->remove(fs->self, temporary)));

        rc = cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped);
        CHECK(rc.code == CC_ERR_STORAGE && !store.self && !store.vtable);
        cc_result_free(&rc);
        content = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, path, &content)));
        CHECK(strcmp(content, invalid[i]) == 0);
        free(content);
        CHECK(ok(fs->vtable->remove(fs->self, path)));
    }

    const char *valid = "[{\"key\":\"a\\\\u0000tail\",\"value\":\"x\\\\u0000tail\",\"category\":\"cat\\\\u0000tail\",\"session_id\":\"s\\\\u0000tail\"},"
        "{\"key\":\"unicode\",\"value\":\"\\u4F60\\u597D\",\"category\":\"fact\",\"managed\":true,\"importance\":50,\"evidence\":\"literal \\\\u0000\"}]";
    CHECK(ok(fs->vtable->write_text(fs->self, path, valid)));
    cc_memory_store_t store = {0};
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped)));
    for (int pass = 0; pass < 2; pass++) {
        cc_memory_entry_t entry = {0};
        CHECK(ok(cc_memory_store_get(&store, "a\\u0000tail", &entry)));
        CHECK(strcmp(entry.value, "x\\u0000tail") == 0);
        CHECK(strcmp(entry.category, "cat\\u0000tail") == 0);
        CHECK(strcmp(entry.session_id, "s\\u0000tail") == 0);
        cc_memory_entry_free(&entry);
        CHECK(entry_is(&store, "unicode", "\xE4\xBD\xA0\xE5\xA5\xBD"));
        cc_memory_plan_t state = {0};
        uint64_t revision = 0;
        CHECK(ok(cc_json_memory_managed_list(&store, &state, &revision)));
        CHECK(state.count == 1 && strcmp(state.records[0].evidence, "literal \\u0000") == 0);
        cc_memory_plan_cleanup(&state);
        if (pass == 0) {
            CHECK(ok(cc_memory_store_set(&store, "safe", "persist", "fact", NULL)));
            cc_memory_store_destroy(&store);
            CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store, path, wrapped)));
        }
    }
    CHECK(entry_is(&store, "safe", "persist"));
    cc_memory_store_destroy(&store);
    CHECK(ok(fs->vtable->remove(fs->self, path)));
    return 0;
}

/**
 * @brief 验证摘要会话、条数与字节限制，以及写入失败、安装后报错和裁剪重试。
 * @param[in] fs 借用的基础文件系统，提供初始夹具写入及测试文件清理。
 * @param[in] wrapped 借用的包装文件系统，其 self 须指向 fault，生命周期覆盖存储和重开。
 * @param[in,out] fault 有效的故障夹具，进入时各开关为零，测试切换 fail_write 和 fail_replace_after，成功返回时均恢复为零。
 * @param[in] path 借用的独立测试文件路径，进入时可覆盖，成功返回前删除。
 * @return 所有断言通过为零，任一断言失败为一。
 * @note 成功时销毁存储并移除夹具文件，断言失败可能保留当前故障开关、对象及文件。
 */
static int test_summary_limits(cc_filesystem_t *fs, cc_filesystem_t *wrapped,
                               fault_fs_t *fault, const char *path)
{
    const char *initial="[{\"key\":\"managed\",\"value\":\"protected\",\"category\":\"context_summary\",\"managed\":true,\"importance\":30}]";
    CHECK(ok(fs->vtable->write_text(fs->self,path,initial)));
    cc_memory_store_t store={0};
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,path,wrapped)));
    CHECK(ok(cc_memory_store_set(&store,"user","user fact","fact",NULL)));
    CHECK(ok(cc_memory_store_set(&store,"a1","111","context_summary","a")));
    CHECK(ok(cc_memory_store_set(&store,"a2","222","context_summary","a")));
    CHECK(ok(cc_memory_store_set(&store,"b1","333","context_summary","b")));
    CHECK(ok(cc_memory_store_set(&store,"a3","444","context_summary","a")));
    CHECK(ok(cc_json_memory_set_summary_cache_limits(&store,2,3,9)));
    CHECK(entry_is(&store,"a1",NULL) && entry_is(&store,"a2","222") &&
          entry_is(&store,"b1","333") && entry_is(&store,"a3","444"));
    CHECK(entry_is(&store,"user","user fact") && entry_is(&store,"managed","protected"));
    /* 重写摘要将其移到数组末尾，同主键的普通事实与受管理记录受到保护。 */
    CHECK(ok(cc_memory_store_set(&store,"a2","222","context_summary","a")));
    cc_result_t rc=cc_memory_store_set(&store,"user","bad","context_summary","a");
    CHECK(rc.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&rc);
    rc=cc_memory_store_set(&store,"managed","bad","context_summary","a");
    CHECK(rc.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&rc);
    rc=cc_memory_store_set(&store,"too-big","1234567890","context_summary","a");
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && entry_is(&store,"too-big",NULL)); cc_result_free(&rc);

    fault->fail_write=1;
    rc=cc_memory_store_set(&store,"b2","555","context_summary","b");
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(entry_is(&store,"b1","333") && entry_is(&store,"a2","222") &&
          entry_is(&store,"a3","444") && entry_is(&store,"b2",NULL));
    fault->fail_write=0;
    CHECK(ok(cc_memory_store_set(&store,"b2","555","context_summary","b")));
    CHECK(entry_is(&store,"b1",NULL) && entry_is(&store,"a2","222") &&
          entry_is(&store,"a3","444") && entry_is(&store,"b2","555"));
    /* 替换已安装但回调报错时，内存须保留已安装的新集合。 */
    fault->fail_replace_after=1;
    rc=cc_memory_store_set(&store,"c1","你好","context_summary","c");
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(entry_is(&store,"c1","你好") && entry_is(&store,"b2","555") &&
          entry_is(&store,"a2",NULL) && entry_is(&store,"a3",NULL));
    int ready=1; CHECK(ok(cc_json_memory_write_ready(&store,&ready)) && !ready);
    /* 限制未要求裁剪时只更新配置，即使已阻断写入也能成功。 */
    CHECK(ok(cc_json_memory_set_summary_cache_limits(&store,2,3,9)));
    rc=cc_json_memory_set_summary_cache_limits(&store,1,1,9);
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(cc_json_memory_set_summary_cache_limits(&store,2,3,9)));
    fault->fail_replace_after=0;
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    CHECK(ok(cc_memory_store_set(&store,"a4","777","context_summary","a")));
    CHECK(entry_is(&store,"c1","你好") && entry_is(&store,"b2",NULL) && entry_is(&store,"a4","777"));

    /* 收紧限制触发裁剪，写入失败时恢复旧记录，恢复写入后可以再次裁剪。 */
    fault->fail_write=1;
    rc=cc_json_memory_set_summary_cache_limits(&store,1,1,9);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(entry_is(&store,"c1","你好") && entry_is(&store,"a4","777"));
    fault->fail_write=0;
    CHECK(ok(cc_json_memory_set_summary_cache_limits(&store,1,1,9)));
    CHECK(entry_is(&store,"c1",NULL) && entry_is(&store,"a4","777") &&
          entry_is(&store,"user","user fact") && entry_is(&store,"managed","protected"));
    cc_memory_store_destroy(&store);
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,path,wrapped)));
    CHECK(entry_is(&store,"a4","777") && entry_is(&store,"managed","protected"));
    cc_memory_store_destroy(&store);
    CHECK(ok(fs->vtable->remove(fs->self,path)));
    return 0;
}

/**
 * @brief 在独立临时目录中运行 JSON 存储恢复、故障注入、摘要限制、空主键及 NUL 边界回归。
 *
 * 覆盖有效时间戳、文件大小预检、临时文件写入和删除失败、替换前后错误、目录同步失败、
 * 外部修改、残留备份或临时目录、损坏与缺失主文件，以及重新打开和原位恢复。
 * @return 全部断言通过为零，任一 CHECK 失败为一并输出行号。
 * @note 成功时销毁存储和文件系统、删除测试目录并释放目录名。失败直接退出，可能保留磁盘夹具供核查。
 * 测试为单线程，包装文件系统借用栈上故障上下文及回调表，均保持到对应存储销毁。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *directory=NULL; CHECK(ok(cc_id_generate_uuid_v4("memory-recovery-", &directory)));
    CHECK(ok(fs.vtable->make_dir(fs.self,directory)));
    char primary[160], backup[165], temporary[165];
    snprintf(primary,sizeof(primary),"%s/memory.json",directory);
    snprintf(backup,sizeof(backup),"%s.bak",primary);
    snprintf(temporary,sizeof(temporary),"%s.tmp",primary);
    const char *valid="[{\"key\":\"k\",\"value\":\"v\",\"created_at\":2200000000,\"updated_at\":2200000001}]";
    CHECK(ok(fs.vtable->write_text(fs.self,primary,valid)));
    cc_memory_store_t store={0}; CHECK(ok(cc_memory_store_create_json_file(&store,primary)));
    cc_memory_entry_t entry={0}; CHECK(ok(cc_memory_store_get(&store,"k",&entry)));
    CHECK(strcmp(entry.value,"v")==0);
    if (sizeof(time_t)>=8U) CHECK((uint64_t)entry.updated_at==2200000001ULL);
    cc_memory_entry_free(&entry); cc_memory_store_destroy(&store);
    fault_fs_t fault={&fs,0,0,0,0,0,0}; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=f_read; vt.write_text=f_write; vt.stat=f_stat; vt.remove=f_remove;
    vt.atomic_replace=f_replace; vt.sync_file=f_sync; vt.sync_dir=f_sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    char empty_key_path[160];
    snprintf(empty_key_path, sizeof(empty_key_path), "%s/empty-key.json", directory);
    CHECK(test_empty_keys(&fs, &wrapped, empty_key_path) == 0);
    char nul_path[160];
    snprintf(nul_path, sizeof(nul_path), "%s/nul-escape.json", directory);
    CHECK(test_nul_escapes(&fs, &wrapped, nul_path) == 0);
#if CC_JSON_MEMORY_MAX_FILE_BYTES > 0
    char *oversized=malloc((size_t)CC_JSON_MEMORY_MAX_FILE_BYTES+2U); CHECK(oversized);
    memset(oversized,'x',(size_t)CC_JSON_MEMORY_MAX_FILE_BYTES+1U);
    oversized[(size_t)CC_JSON_MEMORY_MAX_FILE_BYTES+1U]='\0';
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped)));
    cc_result_t limit=cc_memory_store_set(&store,"k",oversized,NULL,NULL);
    CHECK(limit.code==CC_ERR_LIMIT_EXCEEDED && entry_is(&store,"k","v")); cc_result_free(&limit);
    cc_memory_store_destroy(&store);
    CHECK(ok(fs.vtable->write_text(fs.self,primary,oversized)));
    unsigned before_read=read_calls;
    limit=cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped);
    CHECK(limit.code==CC_ERR_LIMIT_EXCEEDED && !store.self && read_calls==before_read);
    cc_result_free(&limit); free(oversized);
    CHECK(ok(fs.vtable->write_text(fs.self,primary,valid)));
#endif
    char cache_path[160]; snprintf(cache_path,sizeof(cache_path),"%s/cache.json",directory);
    CHECK(test_summary_limits(&fs,&wrapped,&fault,cache_path)==0);
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped)));
    fault.fail_write=1;
    cc_result_t rc=cc_memory_store_set(&store,"k","not saved",NULL,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"v")==0); cc_memory_entry_free(&entry);
    fault.fail_write=0;
    CHECK(ok(cc_memory_store_set(&store,"k","retried",NULL,NULL)));
    fault.fail_write=2; fault.fail_remove=1;
    rc=cc_memory_store_set(&store,"k","partial write",NULL,NULL); CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    int ready=1; CHECK(ok(cc_json_memory_write_ready(&store,&ready)) && !ready);
    char *partial=NULL; CHECK(ok(fs.vtable->read_text(fs.self,temporary,&partial)) && strcmp(partial,"partial evidence")==0); free(partial);
    fault.fail_write=0; fault.fail_remove=0;
    rc=cc_json_memory_recover_in_place(&store); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,temporary)) && ok(cc_json_memory_recover_in_place(&store)));
    fault.fail_replace_before=1;
    rc=cc_memory_store_set(&store,"k","rename retry",NULL,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"retried")==0); cc_memory_entry_free(&entry);
    fault.fail_replace_before=0;
    CHECK(ok(cc_memory_store_set(&store,"k","rename retry",NULL,NULL)));
    fault.fail_replace_after=1;
    rc=cc_memory_store_set(&store,"k","installed",NULL,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"installed")==0); cc_memory_entry_free(&entry);
    rc=cc_memory_store_set(&store,"other","blocked",NULL,NULL);
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    cc_memory_store_destroy(&store); fault.fail_replace_after=0;
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped)));
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"installed")==0); cc_memory_entry_free(&entry);
    fault.fail_dir_sync=1;
    rc=cc_memory_store_set(&store,"k","visible",NULL,NULL);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"visible")==0); cc_memory_entry_free(&entry);
    rc=cc_memory_store_delete(&store,"k"); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    cc_memory_store_destroy(&store); fault.fail_dir_sync=0;
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped)));
    const char *external="[{\"key\":\"external\",\"value\":\"preserved\"}]";
    CHECK(ok(fs.vtable->write_text(fs.self,primary,external)));
    rc=cc_memory_store_set(&store,"k","must not overwrite",NULL,NULL);
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    char *disk=NULL; CHECK(ok(fs.vtable->read_text(fs.self,primary,&disk)) && strcmp(disk,external)==0); free(disk);
    cc_memory_store_destroy(&store);
    CHECK(ok(fs.vtable->write_text(fs.self,primary,valid)));
    CHECK(ok(fs.vtable->write_text(fs.self,backup,valid)));
    CHECK(ok(cc_memory_store_create_json_file_with_filesystem(&store,primary,&wrapped)));
    CHECK(ok(cc_json_memory_write_ready(&store,&ready)) && !ready);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"v")==0); cc_memory_entry_free(&entry);
    rc=cc_memory_store_set(&store,"k","blocked by backup",NULL,NULL); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    rc=cc_json_memory_recover_in_place(&store); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    char *content=NULL; CHECK(ok(fs.vtable->read_text(fs.self,backup,&content)) && strcmp(content,valid)==0); free(content);
    CHECK(ok(fs.vtable->remove(fs.self,backup)));
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    CHECK(ok(cc_json_memory_write_ready(&store,&ready)) && ready);
    CHECK(ok(fs.vtable->make_dir(fs.self,temporary)));
    rc=cc_memory_store_set(&store,"k","blocked by directory",NULL,NULL); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    cc_filesystem_stat_t sidecar_info={.size=sizeof(sidecar_info)};
    CHECK(ok(fs.vtable->stat(fs.self,temporary,&sidecar_info)) && sidecar_info.type==CC_FS_ENTRY_DIRECTORY);
    rc=cc_json_memory_recover_in_place(&store); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,temporary)));
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    fault.retain_backup=1;
    rc=cc_memory_store_set(&store,"k","installed with backup",NULL,NULL); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(&store,"k",&entry)) && strcmp(entry.value,"installed with backup")==0); cc_memory_entry_free(&entry);
    CHECK(ok(cc_json_memory_write_ready(&store,&ready)) && !ready);
    CHECK(ok(fs.vtable->read_text(fs.self,backup,&content)) && strcmp(content,"backup evidence")==0); free(content);
    fault.retain_backup=0; CHECK(ok(fs.vtable->remove(fs.self,backup)));
    CHECK(ok(cc_json_memory_recover_in_place(&store)));
    CHECK(ok(cc_memory_store_set(&store,"k","after review",NULL,NULL)));
    cc_memory_store_destroy(&store);
    CHECK(ok(fs.vtable->write_text(fs.self,backup,valid)));
    CHECK(ok(fs.vtable->write_text(fs.self,primary,"{broken")));
    rc=cc_memory_store_create_json_file(&store,primary);
    CHECK(rc.code!=CC_OK && !store.self); cc_result_free(&rc);
    CHECK(ok(fs.vtable->read_text(fs.self,backup,&content)) && strcmp(content,valid)==0); free(content);
    CHECK(ok(fs.vtable->remove(fs.self,primary)));
    rc=cc_memory_store_create_json_file(&store,primary);
    CHECK(rc.code==CC_ERR_STORAGE && !store.self); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,backup)));
    CHECK(ok(fs.vtable->write_text(fs.self,temporary,valid)));
    rc=cc_memory_store_create_json_file(&store,primary);
    CHECK(rc.code==CC_ERR_STORAGE && !store.self); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,temporary)));
    CHECK(ok(fs.vtable->write_text(fs.self,primary,"[{\"key\":\"good\",\"value\":\"v\"},{\"value\":\"missing key\"}]")));
    rc=cc_memory_store_create_json_file(&store,primary);
    CHECK(rc.code==CC_ERR_STORAGE && !store.self); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,primary)));
    CHECK(ok(cc_memory_store_create_json_file(&store,primary)));
    cc_memory_store_destroy(&store);
    CHECK(ok(fs.vtable->remove(fs.self,directory))); fs.vtable->destroy(fs.self); free(directory);
    return 0;
}

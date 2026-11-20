#include "cc/app/cc_memory_snapshot.h"
#include "cc/core/cc_id.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 断言失败时打印源码行号并立即退出 main，失败现场保留供检查。 */
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/**
 * @brief 检查结果码，打印失败消息并释放该结果拥有的消息。
 * @param[in] rc 按值移交的结果，调用后其中消息资源已释放。
 * @return 结果码为 CC_OK 返回一，其余返回零。
 */
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }
/**
 * @brief 借用真实文件系统的同步故障注入上下文，由 main 在栈上保管。
 *
 * 测试按顺序修改标志并调用快照接口，包装器与上下文在 store 销毁前保持有效。
 */
typedef struct fault_fs {
    cc_filesystem_t *base;           /**< 借用的底层文件系统，其 vtable 覆盖全部转发调用。 */
    int fail_write;                  /**< 非零时写包装器持续返回 CC_ERR_IO，main 显式复位。 */
    int fail_dir;                    /**< 非零时目录同步包装器持续返回 CC_ERR_IO，main 显式复位。 */
    int collide_temporary;           /**< 下一次候选 .tmp 的 stat 前注入文件，触发后清零。 */
    int collide_snapshot;            /**< 下一次候选 .json 的 stat 前注入 .json 与同 ID .tmp，触发后清零。 */
    char collision_temporary[512];   /**< 上次注入的临时路径，供 main 检查内容并清理。 */
    char collision_snapshot[512];    /**< 上次注入的正式快照路径，供 main 检查内容并清理。 */
} fault_fs_t;
/**
 * @brief 将文本读取原样转交底层文件系统。
 * @param[in] s 必填的借用 fault_fs_t 上下文，base 在调用期间有效。
 * @param[in] p 同步借用的文件路径。
 * @param[out] v 必填的文本输出槽，初始化与失败状态沿用底层契约，成功字符串由调用者 free。
 * @return 原样返回底层 read_text 结果，消息由调用者释放。
 */
static cc_result_t rd(void *s,const char *p,char **v) { fault_fs_t *f=s; return f->base->vtable->read_text(f->base->self,p,v); }
/**
 * @brief 按 fail_write 注入磁盘写入失败，其余写入转交底层。
 * @param[in,out] s 必填的借用故障上下文，底层写入会改变真实文件。
 * @param[in] p 同步借用的目标路径。
 * @param[in] v 同步借用的 NUL 结尾文本。
 * @return fail_write 非零返回 CC_ERR_IO，其余返回底层 write_text 结果，消息由调用者释放。
 */
static cc_result_t wr(void *s,const char *p,const char *v) { fault_fs_t *f=s; if (f->fail_write) return cc_result_error(CC_ERR_IO,"injected full disk"); return f->base->vtable->write_text(f->base->self,p,v); }
/**
 * @brief 将目录列举转交底层，供快照列表检查历史文件。
 * @param[in] s 必填的借用故障上下文及 base。
 * @param[in] p 同步借用的目录路径。
 * @param[out] v 必填的名称数组输出槽，成功数组与各字符串由调用者释放，失败状态沿用底层契约。
 * @param[out] n 必填的条数输出槽，初始化及失败状态由底层实现决定。
 * @return 原样返回底层 list_dir 结果，消息由调用者释放。
 */
static cc_result_t list(void *s,const char *p,char ***v,size_t *n) { fault_fs_t *f=s; return f->base->vtable->list_dir(f->base->self,p,v,n); }
/**
 * @brief 在候选路径首次 stat 时注入既有文件，随后返回真实文件状态。
 * @param[in,out] s 必填的借用故障上下文，触发的注入标志先清零并记录路径，创建的文件由 main 清理。
 * @param[in] p 同步借用的被检查路径，按原因前缀及后缀识别随机 ID，本测试生成的路径小于记录缓冲容量。
 * @param[in,out] v 必填的状态结构，调用者先设置 size，底层填写类型、字节数及修改时间。
 * @return 注入写入失败时返回该错误且可能已创建部分文件，其余返回底层 stat 结果，消息由调用者释放。
 */
static cc_result_t st(void *s, const char *p, cc_filesystem_stat_t *v)
{
    fault_fs_t *f=s;
    size_t length=strlen(p);
    int candidate=strstr(p,"/rewrite-")!=NULL || strstr(p,"/reset-")!=NULL;
    if (candidate && f->collide_snapshot && length>=5U && strcmp(p+length-5U,".json")==0) {
        f->collide_snapshot=0;
        snprintf(f->collision_snapshot,sizeof(f->collision_snapshot),"%s",p);
        snprintf(f->collision_temporary,sizeof(f->collision_temporary),"%.*s.tmp",(int)(length-5U),p);
        cc_result_t rc=f->base->vtable->write_text(f->base->self,p,"[]");
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
        rc=f->base->vtable->write_text(f->base->self,f->collision_temporary,"existing temporary");
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
    }
    if (candidate && f->collide_temporary && length>=4U && strcmp(p+length-4U,".tmp")==0) {
        f->collide_temporary=0;
        snprintf(f->collision_temporary,sizeof(f->collision_temporary),"%s",p);
        cc_result_t rc=f->base->vtable->write_text(f->base->self,p,"existing temporary");
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
    }
    return f->base->vtable->stat(f->base->self,p,v);
}
/**
 * @brief 将文件或目录删除转交底层，供快照清理路径执行真实删除。
 * @param[in,out] s 必填的借用故障上下文，操作改变底层文件系统。
 * @param[in] p 同步借用的待删除路径。
 * @return 原样返回底层 remove 结果，消息由调用者释放。
 */
static cc_result_t rm(void *s,const char *p) { fault_fs_t *f=s; return f->base->vtable->remove(f->base->self,p); }
/**
 * @brief 将临时文件到目标文件的原子替换原样转交底层。
 * @param[in,out] s 必填的借用故障上下文，操作改变底层文件系统。
 * @param[in] a 同步借用的临时文件路径。
 * @param[in] b 同步借用的正式快照路径。
 * @return 原样返回底层 atomic_replace 结果及其安装不确定性，消息由调用者释放。
 */
static cc_result_t replace(void *s,const char *a,const char *b) { fault_fs_t *f=s; return f->base->vtable->atomic_replace(f->base->self,a,b); }
/**
 * @brief 将快照临时文件的同步转交底层。
 * @param[in,out] s 必填的借用故障上下文及有效 base。
 * @param[in] p 同步借用的文件路径。
 * @return 原样返回底层 sync_file 结果，消息由调用者释放。
 */
static cc_result_t sync_file(void *s,const char *p) { fault_fs_t *f=s; return f->base->vtable->sync_file(f->base->self,p); }
/**
 * @brief 按 fail_dir 注入目录同步失败，其余同步转交底层。
 * @param[in,out] s 必填的借用故障上下文，fail_dir 由 main 显式设置和复位。
 * @param[in] p 同步借用的快照目录路径。
 * @return fail_dir 非零返回 CC_ERR_IO，其余返回底层 sync_dir 结果，消息由调用者释放。
 */
static cc_result_t sync_dir(void *s,const char *p) { fault_fs_t *f=s; if (f->fail_dir) return cc_result_error(CC_ERR_IO,"injected dir sync failure"); return f->base->vtable->sync_dir(f->base->self,p); }

/**
 * @brief 顺序验证快照创建、来源确认、读取、分类列表、故障现场保留及内容损坏检测。
 *
 * UUID 生成的相对目录隔离本次文件。故障包装器借用栈上文件系统及上下文，依次注入写失败、
 * 既有临时文件冲突、既有快照及同 ID 临时文件冲突、目录同步失败。冲突回归逐字检查原文件，
 * 目录同步失败后用返回的 ID 确认并删除已安装版本。末段篡改快照以检查读与列表校验失败。
 * 成功路径释放全部结果、文本、列表、ID、存储与文件系统，并删除测试文件及目录。
 * @return 全部断言及清理成功返回零，CHECK 失败打印行号并立即返回一，保留当时文件及尚未释放资源。
 */
int main(void)
{
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("snapshot-test-",&root)));
    char history[160], active[160]; snprintf(history,sizeof(history),"%s/history",root); snprintf(active,sizeof(active),"%s/memory.json",root);
    CHECK(ok(fs.vtable->make_dir(fs.self,history)));
    const char *old="[{\"key\":\"a\",\"value\":\"old\"}]", *current="[{\"key\":\"a\",\"value\":\"new\"}]";
    CHECK(ok(fs.vtable->write_text(fs.self,active,old)));
    fault_fs_t fault={0}; fault.base=&fs; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=rd; vt.write_text=wr; vt.list_dir=list; vt.stat=st; vt.remove=rm;
    vt.atomic_replace=replace; vt.sync_file=sync_file; vt.sync_dir=sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    cc_memory_snapshot_store_t *store=NULL;
    CHECK(ok(cc_memory_snapshot_store_open(&wrapped,history,active,&store)));
    char *first=NULL; CHECK(ok(cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_REWRITE,&first)) && first);
    CHECK(ok(cc_memory_snapshot_confirm(store,first)));
    CHECK(ok(fs.vtable->write_text(fs.self,active,current)));
    cc_result_t rc=cc_memory_snapshot_confirm(store,first);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    char *data=NULL; CHECK(ok(cc_memory_snapshot_read(store,first,&data)) && strcmp(data,old)==0); free(data);
    char *second=NULL; CHECK(ok(cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_RESET,&second)) && second);
    cc_memory_snapshot_info_t *items=NULL; size_t count=0;
    CHECK(ok(cc_memory_snapshot_list(store,&items,&count)) && count==2);
    int rewrites=0,resets=0;
    for(size_t i=0;i<count;i++) { rewrites+=items[i].reason==CC_MEMORY_SNAPSHOT_REWRITE; resets+=items[i].reason==CC_MEMORY_SNAPSHOT_RESET; }
    CHECK(rewrites==1 && resets==1); cc_memory_snapshot_info_free_array(items,count);
    fault.fail_write=1; char *failed=NULL;
    rc=cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_REWRITE,&failed);
    CHECK(rc.code==CC_ERR_IO && !failed); cc_result_free(&rc); fault.fail_write=0;
    /* .tmp 冲突发生在本次写入之前，失败清理须保留既有内容。 */
    fault.collide_temporary=1;
    rc=cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_REWRITE,&failed);
    CHECK(rc.code==CC_ERR_STORAGE && !failed && !fault.collide_temporary); cc_result_free(&rc);
    data=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,fault.collision_temporary,&data)) && strcmp(data,"existing temporary")==0);
    free(data);
    CHECK(ok(fs.vtable->remove(fs.self,fault.collision_temporary)));
    /* .json 已存在时尚未检查 .tmp，两份已有文件也均归原有现场。 */
    fault.collide_snapshot=1;
    rc=cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_RESET,&failed);
    CHECK(rc.code==CC_ERR_STORAGE && !failed && !fault.collide_snapshot); cc_result_free(&rc);
    data=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,fault.collision_snapshot,&data)) && strcmp(data,"[]")==0);
    free(data); data=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,fault.collision_temporary,&data)) && strcmp(data,"existing temporary")==0);
    free(data);
    CHECK(ok(fs.vtable->remove(fs.self,fault.collision_snapshot)));
    CHECK(ok(fs.vtable->remove(fs.self,fault.collision_temporary)));
    fault.fail_dir=1; char *uncertain=NULL;
    rc=cc_memory_snapshot_create(store,CC_MEMORY_SNAPSHOT_REWRITE,&uncertain);
    CHECK(rc.code==CC_ERR_IO && uncertain); cc_result_free(&rc); fault.fail_dir=0;
    CHECK(ok(cc_memory_snapshot_confirm(store,uncertain)));
    CHECK(ok(cc_memory_snapshot_delete(store,uncertain))); free(uncertain);
    char first_path[300]; snprintf(first_path,sizeof(first_path),"%s/%s.json",history,first);
    CHECK(ok(fs.vtable->write_text(fs.self,first_path,"[]")));
    rc=cc_memory_snapshot_read(store,first,&data); CHECK(rc.code==CC_ERR_STORAGE && !data); cc_result_free(&rc);
    rc=cc_memory_snapshot_list(store,&items,&count); CHECK(rc.code==CC_ERR_STORAGE && !items); cc_result_free(&rc);
    CHECK(ok(fs.vtable->write_text(fs.self,first_path,old)));
    CHECK(ok(cc_memory_snapshot_delete(store,first)) && ok(cc_memory_snapshot_delete(store,second)));
    free(first); free(second); cc_memory_snapshot_store_destroy(store);
    CHECK(ok(fs.vtable->remove(fs.self,active)) && ok(fs.vtable->remove(fs.self,history)) && ok(fs.vtable->remove(fs.self,root)));
    fs.vtable->destroy(fs.self); free(root); return 0;
}

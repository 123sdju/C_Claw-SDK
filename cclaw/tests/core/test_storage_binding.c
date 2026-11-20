#include "cc/app/cc_storage_binding.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
#ifdef CC_BINDING_TEST_WRAP_FREE
static void *success_messages[128];
static size_t success_messages_live;
static size_t failed_allocation_bytes;

/**
 * @brief 链接器提供的真实 malloc 入口。
 * @param[in] bytes 分配字节数。
 * @return 独占分配或 NULL。
 */
void *__real_malloc(size_t bytes);

/**
 * @brief 在初始检查结束后按精确路径长度注入一次分配失败。
 * @param[in] bytes 本次所需字节数。
 * @return 命中单次故障时为 NULL，其余委托真实分配。
 */
void *__wrap_malloc(size_t bytes)
{
    if (failed_allocation_bytes && bytes == failed_allocation_bytes) {
        failed_allocation_bytes = 0;
        return NULL;
    }
    return __real_malloc(bytes);
}

/**
 * @brief 链接器提供的真实 free 入口。
 * @param[in,out] pointer 可空独占分配。
 * @post 分配归还，旧地址失效。
 */
void __real_free(void *pointer);

/**
 * @brief 统计注入的成功诊断释放，同时委托真实 free。
 * @param[in,out] pointer 可空独占分配，仅登记消息影响计数。
 * @post 对应登记槽清空，所有非空指针按正常 free 归还。
 */
void __wrap_free(void *pointer)
{
    if (pointer) {
        for (size_t i = 0; i < sizeof(success_messages) / sizeof(success_messages[0]); i++) {
            if (success_messages[i] == pointer) {
                success_messages[i] = NULL;
                success_messages_live--;
                break;
            }
        }
    }
    __real_free(pointer);
}
#endif

/**
 * @brief 为成功回调附上拥有诊断，失败结果原样转交。
 * @param[in,out] result 按值接管的结果，成功时先释放旧诊断。
 * @return 新拥有成功消息或原错误，接收者最终 result_free。
 */
static cc_result_t with_success_message(cc_result_t result)
{
    if (result.code != CC_OK) return result;
    cc_result_free(&result);
    result = cc_result_error(CC_OK, "owned callback success");
    assert(result.message);
#ifdef CC_BINDING_TEST_WRAP_FREE
    size_t i = 0;
    while (i < sizeof(success_messages) / sizeof(success_messages[0]) && success_messages[i]) i++;
    assert(i < sizeof(success_messages) / sizeof(success_messages[0]));
    success_messages[i] = result.message;
    success_messages_live++;
#endif
    return result;
}

/**
 * @brief 检查并消费一个独占结果。
 * @param[in,out] rc 按值接管诊断的结果。
 * @return 成功为 1，错误打印诊断后为 0，动态字段始终释放。
 */
static int ok(cc_result_t rc)
{
    int good = rc.code == CC_OK;
    if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error");
    cc_result_free(&rc);
    return good;
}

/** @brief 测试独占的设备身份，支持提交前、提交后及读取错误。 */
typedef struct anchor {
    char *identity;
    int fail_write;
    int fail_read;
    int fail_after_write;
} anchor_t;

/**
 * @brief 从替身设备读取独立身份，注入失败或拥有成功诊断。
 * @param[in] self 存活 anchor 借用。
 * @param[out] out 必填新槽，先置 NULL，成功字符串由接收者 free。
 * @return 缺失、IO、OOM 或成功，诊断交接收者。
 */
static cc_result_t read_anchor(void *self,char **out)
{
    anchor_t *a=self; *out=NULL;
    if (a->fail_read) return cc_result_error(CC_ERR_IO,"injected identity read failure");
    if (!a->identity) return cc_result_error(CC_ERR_NOT_FOUND,"no device identity");
    *out=cc_copy_string(a->identity);
    return *out ? with_success_message(cc_result_ok()) : cc_result_error(CC_ERR_OUT_OF_MEMORY,"cannot copy identity");
}

/**
 * @brief 仅为无身份设备安装初值，允许安装后报告提交错误。
 * @param[in,out] self 独占 anchor 状态。
 * @param[in] id 调用期稳定身份文本。
 * @return 已存在、提交前/后 IO、复制 OOM 或拥有成功诊断。
 */
static cc_result_t write_anchor(void *self,const char *id)
{
    anchor_t *a=self;
    if (a->identity) return cc_result_error(CC_ERR_INVALID_STATE,"identity already exists");
    if (a->fail_write) return cc_result_error(CC_ERR_IO,"injected anchor failure");
    a->identity=cc_copy_string(id);
    if (!a->identity) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"cannot retain identity");
    if (a->fail_after_write) return cc_result_error(CC_ERR_IO,"injected committed anchor failure");
    return with_success_message(cc_result_ok());
}

/** @brief 借用真实文件系统并为同步、替换及调用计数增加控制。 */
typedef struct fault_fs {
    cc_filesystem_t *base;
    int fail_sync;
    int fail_after_replace;
    unsigned write_calls;
    unsigned remove_calls;
#ifdef CC_BINDING_TEST_WRAP_FREE
    int fail_path_allocation;
    size_t main_path_bytes;
#endif
} fault_fs_t;

/**
 * @brief 委托真实读取并包装成功诊断。
 * @param[in] s 存活 fault_fs 借用。
 * @param[in] p 调用期路径借用。
 * @param[out] v 必填拥有文本槽，清理由绑定逻辑负责。
 * @return 真实读取错误或拥有成功结果。
 */
static cc_result_t rd(void *s, const char *p, char **v)
{
    fault_fs_t *f = s;
    return with_success_message(f->base->vtable->read_text(f->base->self, p, v));
}

/**
 * @brief 计数真实写入并包装成功诊断。
 * @param[in,out] s 存活 fault_fs 状态。
 * @param[in] p 调用期路径借用。
 * @param[in] v 调用期正文借用。
 * @return 真实写入错误或拥有成功结果。
 */
static cc_result_t wr(void *s, const char *p, const char *v)
{
    fault_fs_t *f = s;
    f->write_calls++;
    return with_success_message(f->base->vtable->write_text(f->base->self, p, v));
}

/**
 * @brief 委托元数据读取并可在备份检查后设置单次路径分配故障。
 * @param[in] s 存活 fault_fs 借用。
 * @param[in] p 调用期路径借用。
 * @param[out] v 必填元数据槽。
 * @return 真实 stat 错误或拥有成功结果。
 */
static cc_result_t st(void *s, const char *p, cc_filesystem_stat_t *v)
{
    fault_fs_t *f = s;
    cc_result_t rc = f->base->vtable->stat(f->base->self, p, v);
#ifdef CC_BINDING_TEST_WRAP_FREE
    if (rc.code == CC_OK && f->fail_path_allocation && strstr(p, "/storage.id.bak")) {
        failed_allocation_bytes = f->main_path_bytes;
        f->fail_path_allocation = 0;
    }
#endif
    return with_success_message(rc);
}

/**
 * @brief 计数真实移除并包装成功诊断。
 * @param[in,out] s 存活 fault_fs 状态。
 * @param[in] p 调用期路径借用。
 * @return 真实 remove 错误或拥有成功结果。
 */
static cc_result_t rm(void *s, const char *p)
{
    fault_fs_t *f = s;
    f->remove_calls++;
    return with_success_message(f->base->vtable->remove(f->base->self, p));
}

/**
 * @brief 委托实际替换，可在安装完成后返回 IO。
 * @param[in,out] s 存活 fault_fs 状态。
 * @param[in] a 调用期临时路径借用。
 * @param[in] b 调用期目标路径借用。
 * @return 真实错误、安装后 IO 或拥有成功诊断。
 */
static cc_result_t replace(void *s,const char *a,const char *b)
{
    fault_fs_t *f=s; cc_result_t rc=f->base->vtable->atomic_replace(f->base->self,a,b);
    if (rc.code==CC_OK && f->fail_after_replace) { cc_result_free(&rc); return cc_result_error(CC_ERR_IO,"injected post-replacement failure"); }
    return with_success_message(rc);
}

/**
 * @brief 注入同步失败或委托真实文件同步。
 * @param[in] s 存活 fault_fs 借用。
 * @param[in] p 调用期路径借用。
 * @return 注入 IO、真实错误或拥有成功诊断。
 */
static cc_result_t sync_file(void *s, const char *p)
{
    fault_fs_t *f = s;
    if (f->fail_sync) return cc_result_error(CC_ERR_IO, "injected sync failure");
    return with_success_message(f->base->vtable->sync_file(f->base->self, p));
}

/**
 * @brief 委托真实目录同步并包装成功诊断。
 * @param[in] s 存活 fault_fs 借用。
 * @param[in] p 调用期目录路径借用。
 * @return 真实错误或拥有成功结果。
 */
static cc_result_t sync_dir(void *s, const char *p)
{
    fault_fs_t *f = s;
    return with_success_message(f->base->vtable->sync_dir(f->base->self, p));
}

/**
 * @brief 调用只读检查并消费结果诊断。
 * @param[in] fs 存活文件系统借用。
 * @param[in] dir 调用期目录借用。
 * @param[in] device 存活设备端口借用。
 * @param[in] wanted 期望组合状态。
 * @return 成功且状态相同为 1，其余打印实际状态并返回 0。
 */
static int expect(cc_filesystem_t *fs,const char *dir,cc_storage_identity_store_t *device,cc_storage_binding_state_t wanted)
{
    cc_storage_binding_state_t got=0; cc_result_t rc=cc_storage_binding_inspect(fs,dir,device,&got);
    int good=rc.code==CC_OK && got==wanted; if (!good) fprintf(stderr,"binding got %d, expected %d\n",(int)got,(int)wanted);
    cc_result_free(&rc); return good;
}
/**
 * @brief 验证 FAT 词法、五种组合、诊断拥有链和初始化故障现场。
 * @return 全部通过为零，CHECK 失败为一，assert 失败中止进程，正常退出清理目录与全部身份分配。
 */
int main(void)
{
    CHECK(ok(cc_storage_binding_validate_fat_paths("/usb0/workspace", "/usb0/cclaw/recovery")));
    CHECK(ok(cc_storage_binding_validate_fat_path("/usb0/cclaw")));
    CHECK(ok(cc_storage_binding_validate_fat_paths("/usb0/cclaw/recovery-other/", "/usb0/cclaw/recovery")));
    cc_result_t paths=cc_storage_binding_validate_fat_paths("/USB0/CCLAW", "/usb0/cclaw/recovery");
    CHECK(paths.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&paths);
    paths=cc_storage_binding_validate_fat_paths("/usb0/cclaw/RECOVERY/data", "/usb0/cclaw/recovery");
    CHECK(paths.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&paths);
    paths=cc_storage_binding_validate_fat_paths("/", "/usb0/cclaw/recovery");
    CHECK(paths.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&paths);
    const char *bad_paths[]={"relative", "/usb0/./cclaw", "/usb0/cclaw/../other", "/usb0/CCLAW~1", "/usb0/cclaw. ", "/usb0//workspace", "/usb0/中文"};
    for (size_t i=0;i<sizeof(bad_paths)/sizeof(bad_paths[0]);i++) {
        paths=cc_storage_binding_validate_fat_paths(bad_paths[i],"/usb0/cclaw/recovery");
        CHECK(paths.code==CC_ERR_INVALID_ARGUMENT); cc_result_free(&paths);
        paths=cc_storage_binding_validate_fat_path(bad_paths[i]);
        CHECK(paths.code==CC_ERR_INVALID_ARGUMENT); cc_result_free(&paths);
    }
    cc_filesystem_t fs={0}; CHECK(ok(cc_filesystem_get_default(&fs)));
    char *root=NULL; CHECK(ok(cc_id_generate_uuid_v4("binding-test-",&root)) && ok(fs.vtable->make_dir(fs.self,root)));
    char marker[160],temp[160],backup[160],missing[160]; snprintf(marker,sizeof(marker),"%s/storage.id",root); snprintf(temp,sizeof(temp),"%s/storage.id.tmp",root);
    snprintf(backup,sizeof(backup),"%s/storage.id.bak",root);
    snprintf(missing,sizeof(missing),"%s/not-created",root);
    anchor_t anchor={0}; cc_storage_identity_store_t device={sizeof(device),&anchor,read_anchor,write_anchor};
    fault_fs_t fault={.base=&fs}; cc_filesystem_vtable_t vt={0}; vt.size=sizeof(vt);
    vt.read_text=rd; vt.write_text=wr; vt.stat=st; vt.remove=rm; vt.atomic_replace=replace; vt.sync_file=sync_file; vt.sync_dir=sync_dir;
    cc_filesystem_t wrapped=fs; wrapped.self=&fault; wrapped.vtable=&vt;
    CHECK(expect(&wrapped,missing,&device,CC_STORAGE_BINDING_UNBOUND));
#ifdef CC_BINDING_TEST_WRAP_FREE
    CHECK(success_messages_live == 0);
#endif
    cc_filesystem_stat_t missing_stat={.size=sizeof(missing_stat)};
    CHECK(ok(fs.vtable->stat(fs.self,missing,&missing_stat)) && missing_stat.type==CC_FS_ENTRY_MISSING);
    cc_result_t absent = cc_storage_binding_initialize(&wrapped, missing, &device);
    CHECK(absent.code == CC_ERR_STORAGE && !fault.write_calls && !fault.remove_calls);
    cc_result_free(&absent);
    const char *conflicts[] = {temp, backup};
    for (size_t i = 0; i < sizeof(conflicts) / sizeof(conflicts[0]); i++) {
        CHECK(ok(fs.vtable->write_text(fs.self, conflicts[i], "preserved")));
        unsigned writes = fault.write_calls, removes = fault.remove_calls;
        cc_result_t conflict = cc_storage_binding_initialize(&wrapped, root, &device);
        CHECK(conflict.code == CC_ERR_STORAGE);
        cc_result_free(&conflict);
        CHECK(fault.write_calls == writes && fault.remove_calls == removes);
        char *contents = NULL;
        CHECK(ok(fs.vtable->read_text(fs.self, conflicts[i], &contents)));
        CHECK(contents && strcmp(contents, "preserved") == 0);
        free(contents);
        CHECK(ok(fs.vtable->remove(fs.self, conflicts[i])));
    }
#ifdef CC_BINDING_TEST_WRAP_FREE
    fault.main_path_bytes = strlen(marker) + 1;
    fault.fail_path_allocation = 1;
    unsigned writes_before_oom = fault.write_calls, removes_before_oom = fault.remove_calls;
    absent = cc_storage_binding_initialize(&wrapped, root, &device);
    CHECK(absent.code == CC_ERR_OUT_OF_MEMORY && !failed_allocation_bytes);
    cc_result_free(&absent);
    CHECK(fault.write_calls == writes_before_oom && fault.remove_calls == removes_before_oom);
#endif
    CHECK(expect(&wrapped,root,&device,CC_STORAGE_BINDING_UNBOUND));
    cc_storage_binding_state_t state=0; int device_failed=0;
    CHECK(ok(cc_storage_binding_inspect_detailed(&wrapped,root,&device,&state,&device_failed)) &&
          state==CC_STORAGE_BINDING_UNBOUND && !device_failed);
    anchor.fail_read=1;
    cc_result_t rc=cc_storage_binding_inspect_detailed(&wrapped,root,&device,&state,&device_failed);
    CHECK(rc.code==CC_ERR_IO && !state && device_failed); cc_result_free(&rc); anchor.fail_read=0;
    anchor.identity=cc_copy_string("invalid"); CHECK(anchor.identity);
    rc=cc_storage_binding_inspect_detailed(&wrapped,root,&device,&state,&device_failed);
    CHECK(rc.code==CC_ERR_STORAGE && !state && device_failed); cc_result_free(&rc);
    free(anchor.identity); anchor.identity=NULL;
    fault.fail_sync=1; rc=cc_storage_binding_initialize(&wrapped,root,&device);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc); fault.fail_sync=0;
    CHECK(expect(&wrapped,root,&device,CC_STORAGE_BINDING_UNBOUND));
    CHECK(ok(cc_storage_binding_initialize(&wrapped,root,&device)) && expect(&wrapped,root,&device,CC_STORAGE_BINDING_MATCH));
    CHECK(expect(&wrapped,missing,&device,CC_STORAGE_BINDING_DEVICE_ONLY));
    rc=cc_storage_binding_initialize(&wrapped,root,&device); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    char *original=cc_copy_string(anchor.identity); CHECK(original);
    free(anchor.identity); anchor.identity=NULL;
    CHECK(expect(&wrapped,root,&device,CC_STORAGE_BINDING_MEDIA_ONLY));
    CHECK(ok(cc_id_generate_uuid_v4("storage-",&anchor.identity)) && expect(&wrapped,root,&device,CC_STORAGE_BINDING_MISMATCH));
    CHECK(ok(fs.vtable->remove(fs.self,marker)) && expect(&wrapped,root,&device,CC_STORAGE_BINDING_DEVICE_ONLY));
    free(anchor.identity); anchor.identity=original; original=NULL;
    CHECK(ok(fs.vtable->write_text(fs.self,marker,anchor.identity)) && expect(&wrapped,root,&device,CC_STORAGE_BINDING_MATCH));
    CHECK(ok(fs.vtable->write_text(fs.self,temp,"partial")));
    device_failed=1; rc=cc_storage_binding_inspect_detailed(&wrapped,root,&device,&state,&device_failed);
    CHECK(rc.code==CC_ERR_STORAGE && state==0 && !device_failed); cc_result_free(&rc); CHECK(ok(fs.vtable->remove(fs.self,temp)));
    CHECK(ok(fs.vtable->write_text(fs.self,marker,"damaged")));
    rc=cc_storage_binding_inspect(&wrapped,root,&device,&state); CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,marker))); free(anchor.identity); anchor.identity=NULL;

    /* 提交前设备失败与媒体安装后报错保留单侧现场，初始化错误不会被吞掉。 */
    anchor.fail_write=1; device_failed=0;
    rc=cc_storage_binding_initialize_detailed(&wrapped,root,&device,&device_failed);
    CHECK(rc.code==CC_ERR_IO && device_failed); cc_result_free(&rc); anchor.fail_write=0;
    CHECK(expect(&wrapped,root,&device,CC_STORAGE_BINDING_MEDIA_ONLY));
    rc=cc_storage_binding_initialize(&wrapped,root,&device); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self,marker)));
    fault.fail_after_replace=1; rc=cc_storage_binding_initialize(&wrapped,root,&device);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc); fault.fail_after_replace=0;
    CHECK(expect(&wrapped,root,&device,CC_STORAGE_BINDING_MEDIA_ONLY));
    CHECK(ok(fs.vtable->remove(fs.self,marker)));
    anchor.fail_after_write = 1;
    device_failed = 0;
    rc = cc_storage_binding_initialize_detailed(&wrapped, root, &device, &device_failed);
    CHECK(rc.code == CC_ERR_IO && device_failed);
    cc_result_free(&rc);
    anchor.fail_after_write = 0;
    CHECK(expect(&wrapped, root, &device, CC_STORAGE_BINDING_MATCH));
    rc = cc_storage_binding_initialize(&wrapped, root, &device);
    CHECK(rc.code == CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    free(anchor.identity);
    anchor.identity = NULL;
    CHECK(ok(fs.vtable->remove(fs.self,marker)) && ok(fs.vtable->remove(fs.self,root)));
#ifdef CC_BINDING_TEST_WRAP_FREE
    CHECK(success_messages_live == 0);
#endif
    free(root); fs.vtable->destroy(fs.self); return 0;
}

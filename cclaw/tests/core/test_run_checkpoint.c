#include "cc/app/cc_run_checkpoint.h"
#include "cc/core/cc_id.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); return 1; } } while (0)
/**
 * @brief 消费检查点操作结果并打印失败诊断。
 * @param[in] rc 按值移交的拥有结果。
 * @return 成功一，失败零，诊断在返回前释放。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; if (!good) fprintf(stderr, "%s\n", rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }
typedef struct fault_fs { cc_filesystem_t *base; int fail_sync; } fault_fs_t;
/**
 * @brief 转发真实文件文本读取。
 * @param[in] s fault_fs_t 借用，base 覆盖存储寿命。
 * @param[in] p 调用期路径借用。
 * @param[out] v 新拥有文本槽，成功由上层 free。
 * @return 下层拥有结果原样移交，夹具串行使用代理。
 */
static cc_result_t f_read(void *s, const char *p, char **v)
{ fault_fs_t *f = s; return f->base->vtable->read_text(f->base->self, p, v); }
/**
 * @brief 转发完整文本写入。
 * @param[in] s 存活的 fault_fs_t 借用。
 * @param[in] p 调用期路径借用。
 * @param[in] v 调用期正文借用，不保留。
 * @return 下层拥有结果交调用者消费，代理不拥有输入。
 */
static cc_result_t f_write(void *s, const char *p, const char *v)
{ fault_fs_t *f = s; return f->base->vtable->write_text(f->base->self, p, v); }
/**
 * @brief 转发真实目录枚举。
 * @param[in] s 存活的 fault_fs_t 借用。
 * @param[in] p 调用期目录路径借用。
 * @param[out] v 新名称数组槽，成功由上层逐项 free 后 free 数组。
 * @param[out] n 目录条目计数槽。
 * @return 下层结果原样移交，不缓存输出。
 */
static cc_result_t f_list(void *s, const char *p, char ***v, size_t *n)
{ fault_fs_t *f = s; return f->base->vtable->list_dir(f->base->self, p, v, n); }
/**
 * @brief 转发文件元数据查询。
 * @param[in] s 存活代理借用。
 * @param[in] p 调用期路径借用。
 * @param[out] v 调用者独占的元数据槽。
 * @return 下层拥有结果，无额外拥有对象。
 */
static cc_result_t f_stat(void *s, const char *p, cc_filesystem_stat_t *v)
{ fault_fs_t *f = s; return f->base->vtable->stat(f->base->self, p, v); }
/**
 * @brief 转发当前独占夹具目录中的文件删除。
 * @param[in] s 存活代理借用。
 * @param[in] p 调用期路径借用。
 * @return 下层拥有结果，由上层消费。
 */
static cc_result_t f_remove(void *s, const char *p)
{ fault_fs_t *f = s; return f->base->vtable->remove(f->base->self, p); }
/**
 * @brief 转发真实原子替换以保留持久化事务行为。
 * @param[in] s 存活代理借用，base 由主函数拥有。
 * @param[in] a 调用期临时源路径借用。
 * @param[in] b 调用期目标路径借用。
 * @return 下层拥有结果，文件安装行为按真实后端执行。
 */
static cc_result_t f_replace(void *s, const char *a, const char *b)
{ fault_fs_t *f = s; return f->base->vtable->atomic_replace(f->base->self, a, b); }
/**
 * @brief 注入文件同步失败或执行真实同步。
 * @param[in] s 串行更新 fail_sync 的代理借用。
 * @param[in] p 调用期文件路径借用。
 * @return 开关开启时拥有 I/O 错误，否则下层结果，均由上层消费。
 */
static cc_result_t f_sync(void *s, const char *p)
{
    fault_fs_t *f = s;
    if (f->fail_sync) return cc_result_error(CC_ERR_IO, "injected sync failure");
    return f->base->vtable->sync_file(f->base->self, p);
}
/**
 * @brief 转发真实目录同步。
 * @param[in] s 存活代理借用。
 * @param[in] p 调用期目录路径借用。
 * @return 下层拥有结果，不保留路径，代理由主线程串行使用。
 */
static cc_result_t f_sync_dir(void *s, const char *p)
{ fault_fs_t *f = s; return f->base->vtable->sync_dir(f->base->self, p); }

/**
 * @brief 计算夹具 payload 原文的 CRC32，以构造校验正确但字段表示有问题的记录。
 * @param[in] text 非空借用 C 字符串。
 * @return 与检查点格式一致的 CRC32 数值。
 */
static uint32_t fixture_crc32(const char *text)
{
    uint32_t crc = 0xffffffffU;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        crc ^= *p;
        for (unsigned bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)-(int32_t)(crc & 1U));
    }
    return crc ^ 0xffffffffU;
}

/**
 * @brief 重写有效检查点夹具，验证双层 NUL、非法 Unicode 和错误重试字段类型均被拒绝。
 * @param[in,out] fs 借用真实文件系统，读取和暂时改写当前检查点文件。
 * @param[in,out] store 借用有效检查点存储，测试期间无并发运行。
 * @param[in] directory 当前夹具目录，调用期间借用。
 * @param[in] run_id 当前已保存 MODEL 检查点的 ID，调用期间借用。
 * @return 全部断言通过返回零并恢复原文件，失败返回一并保留现场。
 */
static int test_encoded_fields(cc_filesystem_t *fs, cc_run_checkpoint_store_t *store,
                               const char *directory, const char *run_id)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s.json", directory, run_id);
    char *original = NULL;
    CHECK(ok(fs->vtable->read_text(fs->self, path, &original)) && original);
    const struct {
        const char *field;
        const char *encoded_value;
        const char *expected_input;
    } cases[] = {
        {"user_input", "\"do\\u0000hidden\"", NULL},
        {"session_id", "\"session-a\\u0000hidden\"", NULL},
        {"user_message_id", "\"user-message-1\\u0000hidden\"", NULL},
        {"reasoning_content", "\"reason\\u0000hidden\"", NULL},
        {"user_input", "\"do\\uZZZZhidden\"", NULL},
        {"reasoning_content", "\"reason\\u12Q4hidden\"", NULL},
        {"retry_after_call_id", "1", NULL},
        {"retry_after_call_id", NULL, "do the task"},
        {"user_input", "\"do\\\\u0000literal\"", "do\\u0000literal"}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cJSON *envelope = cJSON_Parse(original);
        cJSON *payload_field = cJSON_GetObjectItemCaseSensitive(envelope, "payload");
        CHECK(cJSON_IsString(payload_field));
        cJSON *payload = cJSON_Parse(payload_field->valuestring);
        CHECK(payload);
        if (cases[i].encoded_value) {
            CHECK(cJSON_ReplaceItemInObjectCaseSensitive(payload, cases[i].field,
                                                       cJSON_CreateRaw(cases[i].encoded_value)));
        } else {
            cJSON_DeleteItemFromObjectCaseSensitive(payload, cases[i].field);
        }
        char *payload_text = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        CHECK(payload_text);
        char checksum[9];
        snprintf(checksum, sizeof(checksum), "%08lx", (unsigned long)fixture_crc32(payload_text));
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(envelope, "payload", cJSON_CreateString(payload_text)));
        CHECK(cJSON_ReplaceItemInObjectCaseSensitive(envelope, "crc32", cJSON_CreateString(checksum)));
        cJSON_free(payload_text);
        char *encoded = cJSON_PrintUnformatted(envelope);
        cJSON_Delete(envelope);
        CHECK(encoded && ok(fs->vtable->write_text(fs->self, path, encoded)));
        cc_run_checkpoint_t loaded = {0};
        cc_result_t rc = cc_run_checkpoint_load(store, run_id, &loaded);
        if (cases[i].expected_input) {
            CHECK(rc.code == CC_OK && strcmp(loaded.user_input, cases[i].expected_input) == 0);
        } else {
            CHECK(rc.code == CC_ERR_STORAGE && !loaded.run_id && !loaded.user_input);
            char **ids = NULL;
            size_t count = 0;
            cc_result_t list = cc_run_checkpoint_list(store, &ids, &count);
            CHECK(list.code == CC_ERR_STORAGE && !ids && count == 0);
            cc_result_free(&list);
        }
        cc_result_free(&rc);
        cc_run_checkpoint_cleanup(&loaded);
        char *retained = NULL;
        CHECK(ok(fs->vtable->read_text(fs->self, path, &retained)) && strcmp(retained, encoded) == 0);
        free(retained);
        cJSON_free(encoded);
    }

    /* 外层 payload 字符串解码成 NUL 时，原 CRC 前缀仍正确，也必须拒绝。 */
    cJSON *envelope = cJSON_Parse(original);
    cJSON *payload_field = cJSON_GetObjectItemCaseSensitive(envelope, "payload");
    CHECK(cJSON_IsString(payload_field));
    char *quoted_payload = cJSON_PrintUnformatted(payload_field);
    CHECK(quoted_payload);
    size_t quoted_length = strlen(quoted_payload);
    const char suffix[] = "\\u0000hidden\"";
    char *encoded_payload = malloc(quoted_length + sizeof(suffix));
    CHECK(encoded_payload);
    memcpy(encoded_payload, quoted_payload, quoted_length - 1U);
    memcpy(encoded_payload + quoted_length - 1U, suffix, sizeof(suffix));
    cJSON_free(quoted_payload);
    CHECK(cJSON_ReplaceItemInObjectCaseSensitive(envelope, "payload", cJSON_CreateRaw(encoded_payload)));
    free(encoded_payload);
    char *encoded = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    CHECK(encoded && ok(fs->vtable->write_text(fs->self, path, encoded)));
    cc_run_checkpoint_t loaded = {0};
    cc_result_t rc = cc_run_checkpoint_load(store, run_id, &loaded);
    CHECK(rc.code == CC_ERR_STORAGE && !loaded.run_id);
    cc_result_free(&rc);
    cc_run_checkpoint_cleanup(&loaded);
    cJSON_free(encoded);
    CHECK(ok(fs->vtable->write_text(fs->self, path, original)));
    free(original);
    return 0;
}

/**
 * @brief 验证声明互斥、保存恢复、编码字段边界、备份与临时记录确认流程。
 * @return 全部断言通过返回零，失败返回一，成功路径清理夹具与拥有资源。
 */
int main(void)
{
    cc_filesystem_t fs = {0};
    CHECK(ok(cc_filesystem_get_default(&fs)));
    char *directory = NULL, *run_id = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("checkpoint-test-", &directory)));
    CHECK(ok(cc_id_generate_uuid_v4("run-", &run_id)));
    char *derived=NULL;
    CHECK(ok(cc_run_checkpoint_derive_user_message_id(run_id,&derived)) &&
          strlen(derived)==45U && strncmp(derived,run_id,40U)==0 && strcmp(derived+40,"-user")==0);
    free(derived); derived=(char *)1;
    cc_result_t invalid_id=cc_run_checkpoint_derive_user_message_id("run-invalid",&derived);
    CHECK(invalid_id.code==CC_ERR_INVALID_ARGUMENT && !derived); cc_result_free(&invalid_id);
    CHECK(ok(fs.vtable->make_dir(fs.self, directory)));
    fault_fs_t fault = {&fs, 0};
    cc_filesystem_vtable_t vtable = {0};
    vtable.size = sizeof(vtable);
    vtable.read_text = f_read; vtable.write_text = f_write; vtable.list_dir = f_list;
    vtable.stat = f_stat; vtable.remove = f_remove; vtable.atomic_replace = f_replace;
    vtable.sync_file = f_sync; vtable.sync_dir = f_sync_dir;
    cc_filesystem_t wrapped = fs;
    wrapped.self = &fault; wrapped.vtable = &vtable;
    cc_run_checkpoint_store_t *store = NULL;
    CHECK(ok(cc_run_checkpoint_store_open(&wrapped, directory, &store)));
    char *second_run = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("run-", &second_run)));
    CHECK(ok(cc_run_checkpoint_claim(store, run_id, "session-a")));
    cc_result_t conflict = cc_run_checkpoint_claim(store, second_run, "session-a");
    CHECK(conflict.code == CC_ERR_INVALID_STATE);
    cc_result_free(&conflict);
    CHECK(ok(cc_run_checkpoint_claim(store, second_run, "session-b")));
    CHECK(ok(cc_run_checkpoint_release(store, run_id)));
    CHECK(ok(cc_run_checkpoint_release(store, second_run)));
    conflict = cc_run_checkpoint_release(store, run_id);
    CHECK(conflict.code == CC_ERR_NOT_FOUND);
    cc_result_free(&conflict);
    free(second_run);
    cc_run_checkpoint_t checkpoint = {0};
    checkpoint.run_id = run_id;
    checkpoint.session_id = "session-a";
    checkpoint.user_message_id = "user-message-1";
    checkpoint.user_input = "do the task";
    checkpoint.step = 1;
    checkpoint.phase = CC_RUN_CHECKPOINT_MODEL;
    CHECK(ok(cc_run_checkpoint_save(store, &checkpoint)));
    cc_run_checkpoint_t loaded = {0};
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)));
    CHECK(loaded.phase == CC_RUN_CHECKPOINT_MODEL && loaded.step == 1 && !loaded.recovered_from_backup);
    cc_run_checkpoint_cleanup(&loaded);
    CHECK(test_encoded_fields(&fs, store, directory, run_id) == 0);

    CHECK(ok(cc_tool_call_list_append_values(&checkpoint.planned_calls, "call-1", "write", "{\"a\":1}")));
    CHECK(ok(cc_tool_call_list_append_values(&checkpoint.planned_calls, "call-2", "read", "{\"value\":\"\\u0000\"}")));
    checkpoint.phase = CC_RUN_CHECKPOINT_TOOLS;
    checkpoint.reasoning_content = "reasoning";
    checkpoint.next_call = 1;
    CHECK(ok(cc_run_checkpoint_save(store, &checkpoint)));
    char *saved_arguments = checkpoint.planned_calls.items[1].arguments_json;
    checkpoint.planned_calls.items[1].arguments_json = "{\"value\":\"\\uZZZZ\"}";
    cc_result_t invalid_arguments = cc_run_checkpoint_save(store, &checkpoint);
    CHECK(invalid_arguments.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&invalid_arguments);
    checkpoint.planned_calls.items[1].arguments_json = saved_arguments;
    fault.fail_sync = 1;
    checkpoint.next_call = 2;
    cc_result_t rc = cc_run_checkpoint_save(store, &checkpoint);
    CHECK(rc.code == CC_ERR_IO);
    cc_result_free(&rc);
    fault.fail_sync = 0;
    cc_run_checkpoint_store_destroy(store);
    CHECK(ok(cc_run_checkpoint_store_open(&wrapped, directory, &store)));
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)));
    CHECK(loaded.phase == CC_RUN_CHECKPOINT_TOOLS && loaded.next_call == 1 &&
          loaded.planned_calls.count == 2 && strcmp(loaded.planned_calls.items[1].id, "call-2") == 0 &&
          strcmp(loaded.planned_calls.items[1].arguments_json, "{\"value\":\"\\u0000\"}") == 0);
    cc_run_checkpoint_cleanup(&loaded);
    char **ids = NULL;
    size_t count = 0;
    CHECK(ok(cc_run_checkpoint_list(store, &ids, &count)) && count == 1 && strcmp(ids[0], run_id) == 0);
    free(ids[0]); free(ids);

    char path[160], backup[170];
    snprintf(path, sizeof(path), "%s/%s.json", directory, run_id);
    snprintf(backup, sizeof(backup), "%s/%s.json.bak", directory, run_id);
    char *contents = NULL;
    CHECK(ok(fs.vtable->read_text(fs.self, path, &contents)));
    CHECK(ok(fs.vtable->write_text(fs.self, backup, contents)));
    free(contents);
    CHECK(ok(fs.vtable->write_text(fs.self, path, "corrupted")));
    rc = cc_run_checkpoint_load(store, run_id, &loaded);
    CHECK(rc.code == CC_ERR_STORAGE);
    cc_result_free(&rc);
    CHECK(ok(fs.vtable->remove(fs.self, path)));
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)));
    CHECK(loaded.recovered_from_backup && loaded.next_call == 1);
    cc_run_checkpoint_cleanup(&loaded);
    CHECK(ok(cc_run_checkpoint_list(store, &ids, &count)) && count == 1);
    free(ids[0]); free(ids);
    CHECK(ok(cc_run_checkpoint_remove(store, run_id)));
    CHECK(ok(cc_run_checkpoint_list(store, &ids, &count)) && count == 0);
    free(ids);

    /* 完整的首次检查点可能只留下临时文件，允许读取核对，安装须经用户明确确认。 */
    CHECK(ok(cc_run_checkpoint_save(store, &checkpoint)));
    char temporary[170];
    snprintf(temporary, sizeof(temporary), "%s/%s.tmp", directory, run_id);
    CHECK(ok(fs.vtable->atomic_replace(fs.self, path, temporary)));
    CHECK(ok(cc_run_checkpoint_list(store, &ids, &count)) && count == 1);
    free(ids[0]); free(ids);
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)) && loaded.recovered_from_temporary &&
          !loaded.recovered_from_backup && loaded.next_call == checkpoint.next_call);
    cc_run_checkpoint_cleanup(&loaded);
    CHECK(ok(cc_run_checkpoint_claim(store, run_id, "session-a")));
    rc = cc_run_checkpoint_confirm_temporary(store, run_id);
    CHECK(rc.code == CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_run_checkpoint_release(store, run_id)));
    fault.fail_sync = 1;
    rc = cc_run_checkpoint_confirm_temporary(store, run_id);
    CHECK(rc.code == CC_ERR_IO); cc_result_free(&rc);
    fault.fail_sync = 0;
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)) && loaded.recovered_from_temporary);
    cc_run_checkpoint_cleanup(&loaded);
    CHECK(ok(cc_run_checkpoint_confirm_temporary(store, run_id)));
    CHECK(ok(cc_run_checkpoint_load(store, run_id, &loaded)) && !loaded.recovered_from_temporary);
    cc_run_checkpoint_cleanup(&loaded);
    rc = cc_run_checkpoint_confirm_temporary(store, run_id);
    CHECK(rc.code == CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(fs.vtable->atomic_replace(fs.self, path, temporary)));
    CHECK(ok(cc_run_checkpoint_remove(store, run_id)));
    CHECK(ok(cc_run_checkpoint_list(store, &ids, &count)) && count == 0);
    free(ids);
    cc_run_checkpoint_store_destroy(store);
    cc_tool_call_list_cleanup(&checkpoint.planned_calls);
    CHECK(ok(fs.vtable->remove(fs.self, directory)));
    fs.vtable->destroy(fs.self);
    free(directory); free(run_id);
    return 0;
}

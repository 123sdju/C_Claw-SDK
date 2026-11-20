#include "cc/ports/cc_artifact_store.h"
#include "cc/ports/cc_storage_factory.h"
#include "cc/adapters/cc_json_file_session_store.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t allocation_calls;
static size_t fail_at = SIZE_MAX;

/**
 * @brief 对本翻译单元中的后端字段分配注入单次失败。
 * @param[in] bytes 所需字节数。
 * @return 独占内存，命中故障序号时为 NULL。
 */
static void *storage_malloc(size_t bytes)
{
    if (allocation_calls++ == fail_at) return NULL;
    return malloc(bytes);
}

/**
 * @brief 对后端数组和状态分配注入失败。
 * @param[in] count 元素数。
 * @param[in] bytes 单项字节数。
 * @return 清零拥有数组或 NULL。
 */
static void *storage_calloc(size_t count, size_t bytes)
{
    if (allocation_calls++ == fail_at) return NULL;
    return calloc(count, bytes);
}

/**
 * @brief 对后端扩容注入失败，保留原分配。
 * @param[in,out] pointer 原拥有地址，可为 NULL。
 * @param[in] bytes 新容量字节数。
 * @return 新地址或 NULL，失败时原地址继续有效。
 */
static void *storage_realloc(void *pointer, size_t bytes)
{
    if (allocation_calls++ == fail_at) return NULL;
    return realloc(pointer, bytes);
}

#define malloc storage_malloc
#define calloc storage_calloc
#define realloc storage_realloc
#define cc_memory_artifact_store_create review_artifact_create
#define cc_memory_session_store_create review_session_create
#include "../../adapters/src/storage/cc_memory_artifact_store.c"
#include "../../adapters/src/storage/cc_memory_session_store.c"
#undef malloc
#undef calloc
#undef realloc

static int backend_calls[3];
static int backend_failure;
static const char *backend_path;
static size_t backend_segment_bytes;
static const char *backend_media_dir;
static size_t backend_base64_bytes;

/**
 * @brief 模拟只在成功时交付拥有实例的后端。
 * @param[in] backend 记录调用的后端下标。
 * @param[in] path 调用期路径借用，保存地址供同步断言。
 * @param[out] out 新空描述符，成功填入识别用地址。
 * @return 可控 STORAGE 或成功。
 */
static cc_result_t fake_backend(int backend, const char *path, cc_session_store_t *out)
{
    backend_calls[backend]++;
    backend_path = path;
    if (backend_failure) return cc_result_error(CC_ERR_STORAGE, "Injected backend failure");
    out->self = &backend_calls[backend];
    return cc_result_ok();
}

/**
 * @brief 记录单文件 JSON 工厂选择。
 * @param[in] path 调用期借用路径。
 * @param[out] out 空描述符。
 * @return 替身创建结果。
 */
static cc_result_t fake_json_create(const char *path, cc_session_store_t *out)
{
    return fake_backend(0, path, out);
}

/**
 * @brief 记录 SQLite 工厂选择并保留 NULL 路径。
 * @param[in] path 调用期可空路径。
 * @param[out] out 空描述符。
 * @return 替身创建结果。
 */
cc_result_t review_sqlite_create(const char *path, cc_session_store_t *out)
{
    return fake_backend(1, path, out);
}

/**
 * @brief 捕获分段后端的完整配置实参。
 * @param[in] path 调用期根目录借用。
 * @param[in] segment_bytes 分段阈值字节数。
 * @param[in] media_dir 调用期可空媒体子目录借用。
 * @param[in] base64_bytes 编码预算字节数。
 * @param[out] out 空描述符。
 * @return 替身创建结果。
 */
cc_result_t review_segmented_create(const char *path, size_t segment_bytes,
    const char *media_dir, size_t base64_bytes, cc_session_store_t *out)
{
    backend_segment_bytes = segment_bytes;
    backend_media_dir = media_dir;
    backend_base64_bytes = base64_bytes;
    return fake_backend(2, path, out);
}

#undef CC_STORAGE_MEMORY
#undef CC_STORAGE_JSON_FILE
#undef CC_STORAGE_JSON_SEGMENTED
#undef CC_STORAGE_SQLITE
#define CC_STORAGE_MEMORY 1
#define CC_STORAGE_JSON_FILE 1
#define CC_STORAGE_JSON_SEGMENTED 1
#define CC_STORAGE_SQLITE 1
#define cc_json_file_store_create fake_json_create
#define cc_sqlite_session_store_create review_sqlite_create
#define cc_json_segmented_store_create review_segmented_create
#define cc_storage_factory_create_store review_factory_create
#include "../../adapters/src/storage/cc_storage_factory.c"

/**
 * @brief 检查结果码并消费动态诊断。
 * @param[in,out] result 本次独占结果值，按值接管诊断。
 * @param[in] expected 期望错误码。
 * @post 断言通过后结果动态字段归还。
 */
static void expect_result(cc_result_t result, cc_error_code_t expected)
{
    assert(result.code == expected);
    cc_result_free(&result);
}

/**
 * @brief 设置下一次后端局部分配失败，或关闭故障注入。
 * @param[in] attempt 零起序号，SIZE_MAX 表示关闭。
 * @post 本轮分配计数归零。
 */
static void allocation_failure(size_t attempt)
{
    allocation_calls = 0;
    fail_at = attempt;
}

/**
 * @brief 会话字段任一分配失败均不能发布半记录，后续可正常重试。
 * @post 创建/读取/清理的全部资源归还，故障注入关闭。
 */
static void test_session_create_failure(void)
{
    cc_session_store_t port = {0};
    for (size_t attempt = 0; attempt < 5; attempt++) {
        allocation_failure(attempt);
        expect_result(review_session_create(&port), CC_ERR_OUT_OF_MEMORY);
        assert(!port.self && !port.vtable);
    }
    allocation_failure(SIZE_MAX);
    expect_result(review_session_create(&port), CC_OK);
    cc_memory_session_store_t *store = port.self;
    expect_result(port.vtable->create_session(NULL, "s", "."), CC_ERR_INVALID_ARGUMENT);
    expect_result(port.vtable->create_session(port.self, NULL, "."), CC_ERR_INVALID_ARGUMENT);
    for (size_t attempt = 0; attempt < 2; attempt++) {
        allocation_failure(attempt);
        expect_result(port.vtable->create_session(port.self, "s", "."), CC_ERR_OUT_OF_MEMORY);
        assert(store->session_count == 0);
        assert(!store->sessions[0].id && !store->sessions[0].workspace_dir);
    }
    allocation_failure(SIZE_MAX);
    expect_result(port.vtable->create_session(port.self, "s", NULL), CC_OK);
    expect_result(port.vtable->create_session(port.self, "s", "changed"), CC_OK);
    cc_session_t *sessions = NULL;
    size_t count = 0;
    for (size_t attempt = 0; attempt < 3; attempt++) {
        allocation_failure(attempt);
        expect_result(port.vtable->list_sessions(port.self, &sessions, &count), CC_ERR_OUT_OF_MEMORY);
        assert(!sessions && !count && store->session_count == 1);
    }
    allocation_failure(SIZE_MAX);
    expect_result(port.vtable->list_sessions(port.self, &sessions, &count), CC_OK);
    assert(count == 1 && strcmp(sessions[0].workspace_dir, CC_DEFAULT_WORKSPACE_PATH) == 0);
    cc_session_free_array(sessions, count);
    port.vtable->destroy(port.self);
}

/**
 * @brief 检查媒体替换失败保留旧值，以及过滤、删除、独立副本和容量上界。
 * @post 所有媒体和后端资源释放，局部故障关闭。
 */
static void test_artifact_replacement(void)
{
    cc_artifact_store_t port = {0};
    allocation_failure(0);
    expect_result(review_artifact_create(&port), CC_ERR_OUT_OF_MEMORY);
    assert(!port.self && !port.vtable);
    allocation_failure(SIZE_MAX);
    expect_result(review_artifact_create(&port), CC_OK);
    memory_artifact_store_t *store = port.self;
    cc_media_artifact_t input = {.id = "a", .kind = CC_MEDIA_IMAGE, .mime = "image/png", .path = "old"};
    allocation_failure(0);
    expect_result(port.vtable->put(port.self, NULL, &input), CC_ERR_OUT_OF_MEMORY);
    assert(!store->items && !store->count && !store->capacity);
    allocation_failure(SIZE_MAX);
    expect_result(port.vtable->put(port.self, "one", &input), CC_OK);
    input.path = "new";
    allocation_failure(0);
    expect_result(port.vtable->put(port.self, "two", &input), CC_ERR_OUT_OF_MEMORY);
    allocation_failure(SIZE_MAX);
    cc_media_artifact_t got;
    expect_result(port.vtable->get(port.self, "a", &got), CC_OK);
    assert(strcmp(got.path, "old") == 0);
    cc_media_artifact_cleanup(&got);
    expect_result(port.vtable->put(port.self, "two", &input), CC_OK);
    cc_media_artifact_list_t listed;
    expect_result(port.vtable->list(port.self, "one", &listed), CC_OK);
    assert(listed.count == 0);
    cc_media_artifact_list_cleanup(&listed);
    expect_result(port.vtable->list(port.self, "two", &listed), CC_OK);
    assert(listed.count == 1 && strcmp(listed.items[0].path, "new") == 0);
    expect_result(port.vtable->remove(port.self, "a"), CC_OK);
    assert(strcmp(listed.items[0].path, "new") == 0);
    cc_media_artifact_list_cleanup(&listed);
    memset(&got, 0x7f, sizeof(got));
    expect_result(port.vtable->get(port.self, "missing", &got), CC_ERR_NOT_FOUND);
    assert(!got.id && !got.path);
    cc_media_artifact_cleanup(&got);
    size_t capacity = store->capacity;
    store->count = store->capacity = SIZE_MAX / sizeof(*store->items);
    expect_result(reserve_record(store), CC_ERR_LIMIT_EXCEEDED);
    store->count = 0;
    store->capacity = capacity;
    port.vtable->destroy(port.self);
}

/**
 * @brief 容量字节溢出及批次复制失败不能发布消息前缀。
 * @post 输入消息、读取副本和后端释放，故障注入关闭。
 */
static void test_batch_atomicity(void)
{
    cc_session_store_t port = {0};
    expect_result(review_session_create(&port), CC_OK);
    cc_memory_session_store_t *store = port.self;
    expect_result(ensure_batch_capacity(store, SIZE_MAX / sizeof(*store->messages) + 1, 0, 0), CC_ERR_LIMIT_EXCEEDED);
    expect_result(ensure_batch_capacity(store, 0, SIZE_MAX / sizeof(*store->tool_calls) + 1, 0), CC_ERR_LIMIT_EXCEEDED);
    expect_result(ensure_batch_capacity(store, 0, 0, SIZE_MAX / sizeof(*store->tool_results) + 1), CC_ERR_LIMIT_EXCEEDED);
    cc_message_t *message = NULL;
    expect_result(cc_message_create_text("m", "s", CC_ROLE_USER, "hello", NULL, &message), CC_OK);
    cc_tool_call_t call = {.id = "c", .name = "tool", .arguments_json = "{}"};
    cc_session_record_t records[] = {
        {.type = CC_SESSION_RECORD_MESSAGE, .data.message = message},
        {.type = CC_SESSION_RECORD_TOOL_CALL, .session_id = "s", .data.tool_call = &call}
    };
    for (size_t attempt = 0; attempt < 8; attempt++) {
        allocation_failure(attempt);
        expect_result(port.vtable->append_records(port.self, records, 2), CC_ERR_OUT_OF_MEMORY);
        assert(store->message_count == 0 && store->tool_call_count == 0);
    }
    allocation_failure(SIZE_MAX);
    expect_result(port.vtable->append_records(port.self, records, 2), CC_OK);
    cc_message_destroy(message);
    cc_message_t *loaded = NULL;
    size_t count = 0;
    expect_result(port.vtable->load_messages(port.self, "s", 0, &loaded, &count), CC_OK);
    assert(count == 1);
    cc_message_cleanup(&loaded[0]);
    free(loaded);
    expect_result(port.vtable->clear_session(port.self, "s"), CC_OK);
    assert(!store->message_count && !store->tool_call_count);
    port.vtable->destroy(port.self);
}

/**
 * @brief 工厂准确转发路径与预算，失败不回退其他后端。
 * @post 真实内存实例释放，替身描述符仅供同步识别且不销毁。
 */
static void test_factory_selection(void)
{
    cc_config_t config = {0};
    cc_session_store_t out = {.self = &config};
    expect_result(review_factory_create(NULL, &out), CC_ERR_INVALID_ARGUMENT);
    assert(!out.self && !out.vtable);
    config.storage_type = "sqlite";
    backend_failure = 1;
    expect_result(review_factory_create(&config, &out), CC_ERR_STORAGE);
    assert(!out.self && !out.vtable && backend_calls[1] == 1 && !backend_calls[0]);
    backend_failure = 0;
    expect_result(review_factory_create(&config, &out), CC_OK);
    assert(!backend_path);
    config.storage_type = "json";
    expect_result(review_factory_create(&config, &out), CC_OK);
    assert(strcmp(backend_path, CC_DEFAULT_JSON_FILE_STORAGE_PATH) == 0);
    config.storage_type = "json_segmented";
    config.storage_path = "owned-by-caller";
    config.session_segment_bytes = 123;
    config.session_media_dir = "media";
    config.multimodal.limits.max_base64_bytes = 456;
    expect_result(review_factory_create(&config, &out), CC_OK);
    assert(backend_path == config.storage_path && backend_segment_bytes == 123);
    assert(backend_media_dir == config.session_media_dir && backend_base64_bytes == 456);
    config.storage_type = "memory";
    expect_result(review_factory_create(&config, &out), CC_OK);
    out.vtable->destroy(out.self);
    config.storage_type = "unknown";
    expect_result(review_factory_create(&config, &out), CC_ERR_INVALID_ARGUMENT);
    assert(!out.self && !out.vtable);
}

/**
 * @brief 顺序运行内存记录与工厂的确定故障回归。
 * @return 全部断言通过为零。
 */
int main(void)
{
    test_session_create_failure();
    test_artifact_replacement();
    test_batch_atomicity();
    test_factory_selection();
    puts("memory storage ownership, capacity and factory contracts: PASS");
    return 0;
}

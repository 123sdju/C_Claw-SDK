#include "cc/app/cc_memory_context.h"
#include "cc/core/cc_id.h"
#include "cc/ports/cc_memory_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 创建测试拥有的内存记忆存储。
 * @param[out] out_store 非 NULL 空接口，成功后由测试销毁。
 * @return 创建结果。
 */
extern cc_result_t cc_memory_store_create_inmem(cc_memory_store_t *out_store);
#if CC_MEMORY_STORE_JSON_FILE
/** @brief 创建测试拥有的 JSON 文件记忆存储。
 * @param[out] out_store 非 NULL 空接口，成功后由测试销毁。
 * @param[in] path 借用临时文件路径，测试在销毁后删除该文件。
 * @return 打开或创建结果。
 */
extern cc_result_t cc_memory_store_create_json_file(cc_memory_store_t *out_store, const char *path);
#endif
#if CC_STORAGE_SQLITE
/** @brief 创建测试拥有的 SQLite 记忆存储。
 * @param[out] out_store 非 NULL 空接口，成功后由测试销毁。
 * @param[in] path 借用临时数据库路径，测试在销毁后删除该文件。
 * @return 打开或创建结果。
 */
extern cc_result_t cc_memory_store_create_sqlite(cc_memory_store_t *out_store, const char *path);
#endif

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)
/** @brief 消费调用结果并判断是否成功。
 * @param[in] rc 按值转交的结果，其动态消息在此释放。
 * @return CC_OK 返回 1，其余返回 0。
 */
static int ok(cc_result_t rc) { int good = rc.code == CC_OK; cc_result_free(&rc); return good; }

/** @brief 用超过默认结果上限的摘要记录验证排除分类发生在后端 limit 之前。
 * @param[in,out] store 已创建的借用存储，写入派生摘要、用户偏好和事实记录。
 * @return 普通及自定义短期分类均被排除返回 0，否则返回 1。
 * @post 正常路径释放注入文本，存储中的测试记录由调用者销毁存储时回收。
 */
static int exercise(cc_memory_store_t *store)
{
    CHECK(ok(cc_memory_store_set(store, "preference", "keyword blue", "user_pref", "s1")));
    for (int i = 0; i < 8; i++) {
        char key[32];
        snprintf(key, sizeof(key), "summary%d", i);
        CHECK(ok(cc_memory_store_set(store, key, "keyword summary", "context_summary", "s1")));
        snprintf(key, sizeof(key), "active%d", i);
        CHECK(ok(cc_memory_store_set(store, key, "keyword short term", "active_summary", "s1")));
    }
    char *block = NULL;
    CHECK(ok(cc_memory_context_inject(store, "keyword", &block)));
    CHECK(block && strstr(block, "keyword blue") && !strstr(block, "keyword summary") &&
          !strstr(block, "keyword short term"));
    free(block);
    for (int i = 0; i < 8; i++) {
        char key[32]; snprintf(key, sizeof(key), "custom%d", i);
        CHECK(ok(cc_memory_store_set(store, key, "second custom short term", "custom_short", "s1")));
    }
    CHECK(ok(cc_memory_store_set(store, "other-fact", "second durable fact", "fact", "s1")));
    block = NULL;
    CHECK(ok(cc_memory_context_inject_filtered(store, "second", "custom_short", &block)));
    CHECK(block && strstr(block, "second durable fact") && !strstr(block, "second custom short term"));
    free(block);
    return 0;
}

/** @brief 对启用的内存、JSON 和 SQLite 后端复用分类过滤断言。
 * @return 全部后端通过返回 0，否则返回 1。正常路径销毁存储并删除临时文件。
 */
int main(void)
{
    cc_memory_store_t store = {0};
    CHECK(ok(cc_memory_store_create_inmem(&store)));
    CHECK(exercise(&store) == 0);
    cc_memory_store_destroy(&store);
#if CC_MEMORY_STORE_JSON_FILE
    char *id = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("memory-test-", &id)));
    char path[128];
    snprintf(path, sizeof(path), "%s.json", id);
    free(id);
    CHECK(ok(cc_memory_store_create_json_file(&store, path)));
    CHECK(exercise(&store) == 0);
    cc_memory_store_destroy(&store);
    CHECK(remove(path) == 0);
#endif
#if CC_STORAGE_SQLITE
    char *sqlite_id = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("memory-sqlite-", &sqlite_id)));
    char sqlite_path[128];
    snprintf(sqlite_path, sizeof(sqlite_path), "%s.db", sqlite_id);
    free(sqlite_id);
    CHECK(ok(cc_memory_store_create_sqlite(&store, sqlite_path)));
    CHECK(exercise(&store) == 0);
    cc_memory_store_destroy(&store);
    CHECK(remove(sqlite_path) == 0);
#endif
    return 0;
}

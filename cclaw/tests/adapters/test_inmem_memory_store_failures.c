#include "cc/ports/cc_memory_store.h"
#include "cc/ports/cc_thread.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief 本测试单线程使用的定点故障开关，仅影响下方私有编译副本。 */
static size_t allocation_count, fail_at;
static int fail_mutex;
/** @brief 初始记录时间快照，用于确认失败更新保留全部已有元数据。 */
static time_t original_created_at, original_updated_at;

/**
 * @brief 判断本次私有副本分配是否命中故障序号。
 * @return 命中非零 fail_at 时为一，其他情况为零，每次调用递增 allocation_count。
 */
static int allocation_fails(void)
{
    allocation_count++;
    return fail_at && allocation_count == fail_at;
}

/**
 * @brief 向私有实现注入 malloc 失败。
 * @param[in] bytes 分配字节数。
 * @return 命中故障时为 NULL，其他情况返回真实 malloc 结果，由原拥有者 free。
 */
static void *test_malloc(size_t bytes)
{
    return allocation_fails() ? NULL : malloc(bytes);
}

/**
 * @brief 向私有实现注入 calloc 失败。
 * @param[in] count 元素数量。
 * @param[in] bytes 单个元素字节数。
 * @return 命中故障时为 NULL，其他情况返回真实 calloc 结果，由原拥有者 free。
 */
static void *test_calloc(size_t count, size_t bytes)
{
    return allocation_fails() ? NULL : calloc(count, bytes);
}

/**
 * @brief 向私有实现注入 realloc 失败，失败保持旧缓冲所有权。
 * @param[in,out] pointer 原分配缓冲，语义遵循 realloc。
 * @param[in] bytes 目标字节数。
 * @return 命中故障时为 NULL，其他情况返回真实 realloc 结果。
 */
static void *test_realloc(void *pointer, size_t bytes)
{
    return allocation_fails() ? NULL : realloc(pointer, bytes);
}

/**
 * @brief 验证互斥锁创建失败会传回工厂。
 * @param[out] out_mutex 故障分支置 NULL，正常分支由真实端口初始化。
 * @return 故障为 CC_ERR_OUT_OF_MEMORY，其他情况返回真实 cc_mutex_create 结果。
 */
static cc_result_t test_mutex_create(cc_mutex_t *out_mutex)
{
    if (fail_mutex) {
        *out_mutex = NULL;
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Injected mutex creation failure");
    }
    return cc_mutex_create(out_mutex);
}

/* 为本测试编译独立实现副本，宏只包围实现 include，公共端口、其他测试及生产分配 API 保持原样。 */
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define cc_mutex_create test_mutex_create
#define cc_memory_store_create_inmem test_create_inmem
#include "../../adapters/src/storage/cc_inmem_memory_store.c"
#undef cc_memory_store_create_inmem
#undef cc_mutex_create
#undef realloc
#undef calloc
#undef malloc

/**
 * @brief 断言当前条件，失败输出行号并从当前测试返回一。
 * @param[in] expression 求值一次的断言表达式。
 * @note 失败直接结束测试，成功路径清理所有持有资源。
 */
#define CHECK(expression) do { if (!(expression)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

/**
 * @brief 检查结果码并消费操作结果消息。
 * @param[in] result 按值移交的结果，调用后原消息结束寿命。
 * @param[in] expected 期望结果码。
 * @return 结果码相同为一，其他情况为零。
 */
static int result_is(cc_result_t result, cc_error_code_t expected)
{
    int matches = result.code == expected;
    cc_result_free(&result);
    return matches;
}

/**
 * @brief 重新开始一次局部定点故障序列。
 * @param[in] index 从一开始的失败分配序号，零关闭分配故障。
 * @post allocation_count 清零，fail_at 保存本次序号。
 */
static void fail_allocation(size_t index)
{
    allocation_count = 0;
    fail_at = index;
}

/**
 * @brief 通过公共读取入口确认旧记录全部字段仍有效，并清理输出副本。
 * @param[in] store 借用的待检查存储，调用前已关闭分配故障。
 * @return 旧记录正文、分类、来源及时间均保持原值为一，其他情况为零。
 */
static int original_record_intact(cc_memory_store_t *store)
{
    cc_memory_entry_t entry = {0};
    cc_result_t rc = cc_memory_store_get(store, "stable", &entry);
    int intact = rc.code == CC_OK && entry.key && entry.value && entry.category && entry.session_id &&
        strcmp(entry.key, "stable") == 0 && strcmp(entry.value, "original") == 0 &&
        strcmp(entry.category, "old-category") == 0 && strcmp(entry.session_id, "origin") == 0 &&
        entry.created_at == original_created_at && entry.updated_at == original_updated_at;
    cc_result_free(&rc);
    cc_memory_entry_free(&entry);
    return intact;
}

/**
 * @brief 验证创建、更新、插入、读取及三类结果数组的分配失败保持状态并可重试。
 * @return 全部断言通过为零，失败为一，成功时释放副本并销毁私有测试存储。
 */
int main(void)
{
    cc_memory_store_t store = {0};
    CHECK(result_is(test_create_inmem(NULL), CC_ERR_INVALID_ARGUMENT));
    for (size_t failure = 1; failure <= 2; failure++) {
        fail_allocation(failure);
        CHECK(result_is(test_create_inmem(&store), CC_ERR_OUT_OF_MEMORY));
        CHECK(!store.self && !store.vtable);
    }
    fail_allocation(0);
    fail_mutex = 1;
    CHECK(result_is(test_create_inmem(&store), CC_ERR_OUT_OF_MEMORY));
    CHECK(!store.self && !store.vtable);
    fail_mutex = 0;
    CHECK(result_is(test_create_inmem(&store), CC_OK));
    CHECK(result_is(cc_memory_store_set(&store, "stable", "original", "old-category", "origin"), CC_OK));
    cc_memory_entry_t baseline = {0};
    CHECK(result_is(cc_memory_store_get(&store, "stable", &baseline), CC_OK));
    original_created_at = baseline.created_at;
    original_updated_at = baseline.updated_at;
    cc_memory_entry_free(&baseline);

    for (size_t failure = 1; failure <= 2; failure++) {
        fail_allocation(failure);
        CHECK(result_is(cc_memory_store_set(&store, "stable", "replacement", "new-category", "other"), CC_ERR_OUT_OF_MEMORY));
        fail_allocation(0);
        CHECK(original_record_intact(&store));
    }
    for (size_t failure = 1; failure <= 4; failure++) {
        fail_allocation(failure);
        CHECK(result_is(cc_memory_store_set(&store, "new", "new-value", "category", "session"), CC_ERR_OUT_OF_MEMORY));
        fail_allocation(0);
        cc_memory_entry_t missing = {0};
        CHECK(result_is(cc_memory_store_get(&store, "new", &missing), CC_ERR_NOT_FOUND));
        CHECK(!missing.key && !missing.value && !missing.category && !missing.session_id);
        CHECK(original_record_intact(&store));
    }
    for (size_t failure = 1; failure <= 4; failure++) {
        cc_memory_entry_t entry = {0};
        fail_allocation(failure);
        CHECK(result_is(cc_memory_store_get(&store, "stable", &entry), CC_ERR_OUT_OF_MEMORY));
        CHECK(!entry.key && !entry.value && !entry.category && !entry.session_id);
        cc_memory_entry_free(&entry);
        fail_allocation(0);
        CHECK(original_record_intact(&store));
    }

    for (int i = 0; i < INITIAL_CAP - 1; i++) {
        char key[32];
        snprintf(key, sizeof(key), "entry-%d", i);
        CHECK(result_is(cc_memory_store_set(&store, key, "value", "category", "session"), CC_OK));
    }
    cc_inmem_memory_store_t *private_store = store.self;
    cc_memory_entry_t *original_slots = private_store->entries;
    fail_allocation(5);
    CHECK(result_is(cc_memory_store_set(&store, "growth", "value", "category", "session"), CC_ERR_OUT_OF_MEMORY));
    CHECK(private_store->entries == original_slots && private_store->count == INITIAL_CAP && private_store->cap == INITIAL_CAP);
    fail_allocation(0);
    CHECK(original_record_intact(&store));
    CHECK(result_is(cc_memory_store_set(&store, "growth", "value", "category", "session"), CC_OK));

    const size_t failures[] = {1, 2, 3, 4, 5, 66};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        cc_memory_entry_t *entries = (cc_memory_entry_t *)(uintptr_t)1;
        size_t count = 99;
        fail_allocation(failures[i]);
        CHECK(result_is(cc_memory_store_search(&store, "", 0, &entries, &count), CC_ERR_OUT_OF_MEMORY));
        CHECK(!entries && !count);
        fail_allocation(0);
        CHECK(original_record_intact(&store));
        entries = (cc_memory_entry_t *)(uintptr_t)1;
        count = 99;
        fail_allocation(failures[i]);
        CHECK(result_is(cc_memory_store_list(&store, NULL, 0, &entries, &count), CC_ERR_OUT_OF_MEMORY));
        CHECK(!entries && !count);
        fail_allocation(0);
        cc_memory_search_result_t *results = (cc_memory_search_result_t *)(uintptr_t)1;
        count = 99;
        cc_memory_query_t query = {.size = sizeof(query), .limit = 0};
        fail_allocation(failures[i]);
        CHECK(result_is(cc_memory_store_query(&store, &query, &results, &count), CC_ERR_OUT_OF_MEMORY));
        CHECK(!results && !count);
        fail_allocation(0);
        CHECK(original_record_intact(&store));
    }
    cc_memory_entry_t *entries = NULL;
    size_t count = 0;
    CHECK(result_is(cc_memory_store_search(&store, "", 0, &entries, &count), CC_OK));
    CHECK(count == INITIAL_CAP + 1);
    cc_memory_entry_free_array(entries, count);
    entries = NULL;
    CHECK(result_is(cc_memory_store_list(&store, NULL, 0, &entries, &count), CC_OK));
    CHECK(count == INITIAL_CAP + 1);
    cc_memory_entry_free_array(entries, count);
    cc_memory_store_destroy(&store);
    return 0;
}





#ifndef CC_MEMORY_STORE_H
#define CC_MEMORY_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_memory_entry.h"
#include <stddef.h>


/* memory store vtable 前置声明，具体函数集合在下方定义。 */
typedef struct cc_memory_store_vtable cc_memory_store_vtable_t;

/*
 * memory store port 句柄。
 *
 * self 指向具体 adapter 实例，vtable 指向该 adapter 的函数表。这个结构是 C 语言
 * OOP 的典型写法：核心层只依赖接口，不依赖 SQLite、JSON 文件或内存实现。
 */
typedef struct cc_memory_store cc_memory_store_t;

/*
 * 结构化 memory 查询条件。
 *
 * size 用于未来扩展；所有字符串只在调用期间借用，store 不应保存指针。category 和
 * session_id 为空时忽略过滤，limit 由 adapter 决定如何解释 0 或负数。
 */
typedef struct cc_memory_query {
    size_t size;
    /* 所有字符串均在调用期间借用；空字符串或 NULL 表示不启用该项过滤。 */
    const char *query;
    const char *category;
    const char *session_id;
    int limit;
} cc_memory_query_t;

/*
 * memory query 返回项。
 *
 * entry 字段由调用方拥有，必须通过 cc_memory_search_result_free_array() 释放；
 * score 用于向量/全文检索 adapter 表达相关度，fallback search 会给默认分数。
 */
typedef struct cc_memory_search_result {
    size_t size;
    /* entry 字段由调用方拥有，必须通过 cc_memory_search_result_free_array() 释放。 */
    cc_memory_entry_t entry;
    double score;
} cc_memory_search_result_t;


/* 具体 memory store 对象：self + vtable 组合形成接口对象。 */
struct cc_memory_store {
    void *self;

    const cc_memory_store_vtable_t *vtable;
};


/*
 * memory store vtable。
 *
 * adapter 必须实现基础 set/get/search/list/delete/destroy；query 是新检索端口，允许
 * 下游接入 embedding/vector store。如果 query 为 NULL，核心 wrapper 会用 search
 * 做兼容 fallback 并在内存中应用 category/session 过滤。
 */
struct cc_memory_store_vtable {


    /* 写入或覆盖一条记忆；key/value 为必填，category/session_id 可选。 */
    cc_result_t (*set)(
        void *self,
        const char *key,
        const char *value,
        const char *category,
        const char *session_id
    );



    /* 按 key 读取单条记忆；out_entry 成功后由调用方释放字段。 */
    cc_result_t (*get)(
        void *self,
        const char *key,
        cc_memory_entry_t *out_entry
    );



    /* 简单文本检索接口；返回 entry 数组由调用方 cc_memory_entry_free_array()。 */
    cc_result_t (*search)(
        void *self,
        const char *query,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );



    /* 按 category 列举记忆；category 可为 NULL 表示不限定分类。 */
    cc_result_t (*list)(
        void *self,
        const char *category,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );



    /* 删除单条记忆；key 为必填。 */
    cc_result_t (*delete_entry)(
        void *self,
        const char *key
    );



    /* 删除某一分类下的记忆；由 adapter 定义是否跨 session。 */
    cc_result_t (*delete_by_category)(
        void *self,
        const char *category
    );



    /* 销毁 adapter self；由 cc_memory_store_destroy() 调用。 */
    void (*destroy)(void *self);



    /* 结构化检索接口；支持 query/category/session/top-k/score。 */
    cc_result_t (*query)(
        void *self,
        const cc_memory_query_t *query,
        cc_memory_search_result_t **out_results,
        size_t *out_count
    );
};



/* 写入 memory；wrapper 统一做空指针和必填字段校验。 */
cc_result_t cc_memory_store_set(
    cc_memory_store_t *store,
    const char *key,
    const char *value,
    const char *category,
    const char *session_id
);

/* 按 key 读取 memory；out_entry 成功后由调用方 cc_memory_entry_free()。 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
);

/* 简单文本检索；返回 entries 数组由调用方 cc_memory_entry_free_array()。 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/* 结构化检索；优先调用 vtable->query，否则 fallback 到 search 后过滤。 */
cc_result_t cc_memory_store_query(
    cc_memory_store_t *store,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
);

/* 释放 query 结果数组和每个 entry 字段。 */
void cc_memory_search_result_free_array(
    cc_memory_search_result_t *results,
    size_t count
);

/* 按分类列举 memory；返回 entries 数组由调用方释放。 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/* 删除单条 memory。 */
cc_result_t cc_memory_store_delete(
    cc_memory_store_t *store,
    const char *key
);

/* 删除分类下的 memory。 */
cc_result_t cc_memory_store_delete_category(
    cc_memory_store_t *store,
    const char *category
);

/* 销毁 memory store adapter；不会释放 cc_memory_store_t 容器本身。 */
void cc_memory_store_destroy(cc_memory_store_t *store);

#endif

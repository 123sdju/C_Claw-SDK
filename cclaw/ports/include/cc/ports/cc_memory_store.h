



#ifndef CC_MEMORY_STORE_H
#define CC_MEMORY_STORE_H

#include "cc/core/cc_result.h"
#include "cc/core/cc_memory_entry.h"
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 记忆存储方法表的前置声明。 */
typedef struct cc_memory_store_vtable cc_memory_store_vtable_t;

/** @brief 后端实例和方法表描述符，按值复制仅产生借用别名，唯一 owner 负责销毁。 */
typedef struct cc_memory_store cc_memory_store_t;

/** @brief 同步结构化查询条件，调用者维持整个基础结构及字符串覆盖查询期。 */
typedef struct cc_memory_query {
    size_t size; /**< 已提供布局字节数，零兼容旧基础调用，正值至少覆盖 limit，尾字段按 size 探测。 */
    const char *query; /**< 文本借用，NULL 转空查询，空串匹配范围由后端决定。 */
    const char *category; /**< 可选分类借用，NULL/空串忽略过滤。 */
    const char *session_id; /**< 可选会话借用，NULL/空串忽略过滤。 */
    int limit; /**< 正值为过滤后条数上限，非正值的原生范围由后端决定。 */
    const char *exclude_category; /**< 可选排除分类借用，size 覆盖本字段时在条数截取前应用。 */
    const char *exclude_category2; /**< 第二个可选排除分类借用，同样按 size 探测并先于截取。 */
} cc_memory_query_t;

/** @brief 读取布局已提供的首个排除分类。 @param[in] query 条件借用，可为 NULL。 @return 字段借用，布局不足或 NULL 查询返回 NULL。 */
static inline const char *cc_memory_query_excluded_category(const cc_memory_query_t *query)
{
    if (!query || query->size < offsetof(cc_memory_query_t, exclude_category) +
        sizeof(query->exclude_category)) return NULL;
    return query->exclude_category;
}

/** @brief 读取布局已提供的第二排除分类。 @param[in] query 条件借用，可为 NULL。 @return 字段借用，布局不足或 NULL 查询返回 NULL。 */
static inline const char *cc_memory_query_second_excluded_category(const cc_memory_query_t *query)
{
    if (!query || query->size < offsetof(cc_memory_query_t, exclude_category2) +
        sizeof(query->exclude_category2)) return NULL;
    return query->exclude_category2;
}

/** @brief 判断分类是否精确匹配任一已启用排除项。 @param[in] query 可选条件借用。 @param[in] category 可选分类借用。 @return 匹配非空排除项为一，NULL 分类或无匹配为零。 */
static inline int cc_memory_query_excludes_category(const cc_memory_query_t *query, const char *category)
{
    if (!category) return 0;
    const char *first = cc_memory_query_excluded_category(query);
    const char *second = cc_memory_query_second_excluded_category(query);
    return (first && first[0] && strcmp(first, category) == 0) ||
           (second && second[0] && strcmp(second, category) == 0);
}

/** @brief 结构化查询拥有结果，数组和字段一起交给 cc_memory_search_result_free_array。 */
typedef struct cc_memory_search_result {
    size_t size; /**< 后端填写的结果布局大小。 */
    cc_memory_entry_t entry; /**< 拥有的条目字段，单项不得与其他条目共享释放责任。 */
    double score; /**< 后端定义的相关度，核心回退固定为 1.0。 */
} cc_memory_search_result_t;


/** @brief 由具体工厂交付的端口，调用者协调描述符与所有浅借用的寿命。 */
struct cc_memory_store {
    void *self; /**< 后端状态，工厂成功时交付唯一销毁责任，部分无状态替身允许 NULL。 */

    const cc_memory_store_vtable_t *vtable; /**< 覆盖调用期的稳定方法表借用。 */
};


/**
 * @brief 记忆后端同步方法表，各方法的并发及持久化原子性由实现提供。
 * @note 字符串和查询仅调用期借用，返回 cc_result_t 动态诊断交调用方释放。
 * 输出槽初始为空，后端失败时须保持空或提供可按对应 count 安全释放的部分拥有数组，禁止悬空字段。
 * 核心包装会回收失败输出，直接调用方法者承担同等清理责任，owner 销毁前须排空所有调用。
 */
struct cc_memory_store_vtable {


    /**
     * @brief 写入或覆盖单条记忆，具体状态提交由后端负责。
     * @param[in,out] self 后端状态借用。
     * @param[in] key 必需键借用，空串范围由后端决定。
     * @param[in] value 必需正文借用。
     * @param[in] category 可选分类借用。
     * @param[in] session_id 可选来源会话借用。
     * @return 拥有操作结果，失败可能已有部分外部副作用。
     */
    cc_result_t (*set)(
        void *self,
        const char *key,
        const char *value,
        const char *category,
        const char *session_id
    );



    /**
     * @brief 深拷贝指定键的条目。
     * @param[in] self 后端状态借用。
     * @param[in] key 必需键借用。
     * @param[out] out_entry 必需空槽，成功字段用 cc_memory_entry_free 归还，失败字段须可安全清理。
     * @return 成功、缺失、分配或后端拥有错误。
     */
    cc_result_t (*get)(
        void *self,
        const char *key,
        cc_memory_entry_t *out_entry
    );



    /**
     * @brief 按文本检索并交付拥有条目数组。
     * @param[in] self 后端状态借用。
     * @param[in] query 必需文本借用，空串范围由后端决定。
     * @param[in] limit 条数上限，非正值由后端解释。
     * @param[out] out_entries 必需空槽，成功数组用 cc_memory_entry_free_array 释放。
     * @param[out] out_count 必需槽，有效拥有条目数，部分失败输出也须准确。
     * @return 拥有结果，成功非零数量必须对应有效数组。
     */
    cc_result_t (*search)(
        void *self,
        const char *query,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );



    /**
     * @brief 按分类列举拥有条目。
     * @param[in] self 后端状态借用。
     * @param[in] category 可选分类，空范围由后端解释。
     * @param[in] limit 条数上限，非正值由后端解释。
     * @param[out] out_entries 必需空槽，成功数组及字段由调用方释放。
     * @param[out] out_count 必需槽，成功或部分失败数组的可释放条目数。
     * @return 拥有结果，成功非零数量必须对应有效数组。
     */
    cc_result_t (*list)(
        void *self,
        const char *category,
        int limit,
        cc_memory_entry_t **out_entries,
        size_t *out_count
    );



    /** @brief 删除指定键。 @param[in,out] self 后端借用。 @param[in] key 必需键借用。 @return 后端拥有结果，缺失和失败后的状态依实现。 */
    cc_result_t (*delete_entry)(
        void *self,
        const char *key
    );



    /** @brief 删除分类记录。 @param[in,out] self 后端借用。 @param[in] category 必需分类借用。 @return 后端拥有结果，跨会话范围及批次原子性依实现。 */
    cc_result_t (*delete_by_category)(
        void *self,
        const char *category
    );



    /** @brief 释放后端拥有资源。 @param[in] self 交接唯一状态。 @pre 所有调用及借用已退出。 @post 后端资源释放，描述符容器由外层管理。 */
    void (*destroy)(void *self);



    /**
     * @brief 在条数截取前应用全部结构化过滤并交付拥有结果。
     * @param[in] self 后端状态借用。
     * @param[in] query 必需条件借用，排除尾字段按 size 探测。
     * @param[out] out_results 必需空槽，成功用 cc_memory_search_result_free_array 释放。
     * @param[out] out_count 必需槽，成功或部分失败输出的安全可释放条目数。
     * @return 后端拥有结果，排序和分值由后端定义，成功非零数量对应有效数组。
     */
    cc_result_t (*query)(
        void *self,
        const cc_memory_query_t *query,
        cc_memory_search_result_t **out_results,
        size_t *out_count
    );
};



/**
 * @brief 校验入口并委托写入，保持后端错误与提交语义。
 * @param[in,out] store 必需存活端口借用。
 * @param[in] key 必需键借用，空串由后端解释。
 * @param[in] value 必需正文借用。
 * @param[in] category 可选分类借用。
 * @param[in] session_id 可选来源会话借用。
 * @return 参数错误或后端拥有结果，调用方 cc_result_free，失败可能已经改变存储。
 */
cc_result_t cc_memory_store_set(
    cc_memory_store_t *store,
    const char *key,
    const char *value,
    const char *category,
    const char *session_id
);

/**
 * @brief 读取条目并统一清理失败输出。
 * @param[in] store 必需存活端口借用。
 * @param[in] key 必需键借用。
 * @param[out] out_entry 必需新或已清理槽，入口清零，成功由调用方 cc_memory_entry_free。
 * @return 参数或后端拥有结果，失败输出为空，诊断由调用方释放。
 */
cc_result_t cc_memory_store_get(
    cc_memory_store_t *store,
    const char *key,
    cc_memory_entry_t *out_entry
);

/**
 * @brief 委托文本检索，失败撤销部分数组。
 * @param[in] store 必需存活端口借用。
 * @param[in] query 必需文本借用，空串匹配范围由后端决定。
 * @param[in] limit 条数上限，非正值沿后端规则。
 * @param[out] out_entries 必需空拥有槽，入口置 NULL，成功用 cc_memory_entry_free_array 释放。
 * @param[out] out_count 必需数量槽，入口置零。
 * @return 参数或后端拥有结果，成功数组形状错误返回 INVALID_STATE，失败输出为空。
 */
cc_result_t cc_memory_store_search(
    cc_memory_store_t *store,
    const char *query,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/**
 * @brief 结构化查询，优先原生方法，缺失或原生忽略排除项时回退 search 后过滤。
 * @param[in] store 必需存活端口借用。
 * @param[in] query 必需完整基础条件，size=0 兼容旧用法，正值须覆盖 limit，尾字段按布局探测。
 * @param[out] out_results 必需空拥有槽，入口置 NULL，成功用 cc_memory_search_result_free_array 释放。
 * @param[out] out_count 必需数量槽，入口置零，成功才发布。
 * @return 参数、UNSUPPORTED、OOM、形状错误或后端拥有结果，失败输出为空，诊断由调用方释放。
 * @note 回退先取候选再过滤分类/会话/排除项及正 limit，score 固定 1，内存占用受全部候选数影响。
 * 旧 search 的非正 limit 范围仍由后端提供，两次原生/回退调用间的一致性由后端和 owner 协调。
 */
cc_result_t cc_memory_store_query(
    cc_memory_store_t *store,
    const cc_memory_query_t *query,
    cc_memory_search_result_t **out_results,
    size_t *out_count
);

/** @brief 消费结构化查询数组。 @param[in] results 独占堆数组，NULL 安全。 @param[in] count 已初始化且可释放的条目数。 @post 字段和数组释放，全部借用须已退出，调用者清空自身悬空指针。 */
void cc_memory_search_result_free_array(
    cc_memory_search_result_t *results,
    size_t count
);

/**
 * @brief 委托分类列表，失败撤销部分拥有数组。
 * @param[in] store 必需存活端口借用。
 * @param[in] category 可选分类借用，空范围沿后端规则。
 * @param[in] limit 条数上限，非正值沿后端规则。
 * @param[out] out_entries 必需空槽，入口置 NULL，成功用 cc_memory_entry_free_array 释放。
 * @param[out] out_count 必需计数槽，入口置零。
 * @return 参数、形状或后端拥有结果，失败输出为空，诊断由调用方释放。
 */
cc_result_t cc_memory_store_list(
    cc_memory_store_t *store,
    const char *category,
    int limit,
    cc_memory_entry_t **out_entries,
    size_t *out_count
);

/** @brief 委托删除单键。 @param[in,out] store 必需存活端口借用。 @param[in] key 必需键借用。 @return 参数或后端拥有结果，失败可能已有副作用，调用方释放诊断。 */
cc_result_t cc_memory_store_delete(
    cc_memory_store_t *store,
    const char *key
);

/** @brief 委托分类删除。 @param[in,out] store 必需存活端口借用。 @param[in] category 必需分类借用。 @return 参数或后端拥有结果，范围与事务性由后端决定，调用方释放诊断。 */
cc_result_t cc_memory_store_delete_category(
    cc_memory_store_t *store,
    const char *category
);

/** @brief 消费后端并清空可销毁描述符。 @param[in,out] store 唯一 owner 提供的对象，NULL 或缺销毁方法时直接返回。 @pre owner 已排空调用与浅借用。 @post 有 destroy 时后端释放且两字段置空，容器继续归调用者。 */
void cc_memory_store_destroy(cc_memory_store_t *store);

#ifdef __cplusplus
}
#endif

#endif

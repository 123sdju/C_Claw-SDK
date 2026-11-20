



#ifndef CC_MEMORY_ENTRY_H
#define CC_MEMORY_ENTRY_H

#include <stddef.h>
#include <time.h>

/**
 * @brief 记忆存储返回的一条拥有独立字符串的键值记录。
 * key、value、category 和 session_id 的非空字段分别拥有可由 free 释放的存储。
 * 适配器输出独立副本，调用者用 cc_memory_entry_free 或数组清理入口归还。
 * created_at 与 updated_at 使用平台提供的 time_t，具体时钟来源由存储实现决定。
 * 结构直接赋值会共享字符串指针，交接拥有权后须清空原拥有者，复制内容须深拷贝。
 */
typedef struct cc_memory_entry {
    char *key;

    char *value;

    char *category;

    char *session_id;

    time_t created_at;

    time_t updated_at;

} cc_memory_entry_t;

/**
 * @brief 将新条目或已清理条目初始化为空状态。
 * @pre 非空条目没有仍需释放的资源，由调用者独占。
 * @param[out] entry 借用的结构地址，允许 NULL，支持栈对象和数组元素。
 * @post 非空条目全部字段清零，结构存储继续归调用者。
 */
void cc_memory_entry_init(cc_memory_entry_t *entry);

/**
 * @brief 清理一条记忆拥有的字符串并清零结构。
 * @pre 条目已初始化，字符串分别拥有且可释放，所有字段借用者已退出。
 * @param[in,out] entry 借用的结构地址，允许 NULL。
 * @post 非空条目的字段已释放且清零，结构存储仍归调用者，空状态可重复清理。
 */
void cc_memory_entry_free(cc_memory_entry_t *entry);

/**
 * @brief 归还记忆查询结果的条目字段和整个数组。
 * @pre 非空数组可释放，前 count 项已初始化且字段拥有权独立，所有借用已结束。
 * @param[in] entries 消费拥有的堆数组，允许 NULL。
 * @param[in] count 有效条目数量，须不超过实际数组长度，NULL 数组时忽略。
 * @post 非空数组及指定条目字段已释放，调用者负责清空自己的数组指针。
 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count);

#endif





#include "cc/core/cc_memory_entry.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief 把新建或已清理的记忆条目置为可继续填充和释放的空状态。
 * @pre 非空 entry 内没有尚需释放的拥有字段，调用者独占该条目。
 * @param[out] entry 借用的条目地址，允许 NULL，此时直接返回。
 * @post 非空条目的全部字段清零，函数不分配资源，原条目存储仍归调用者。
 */
void cc_memory_entry_init(cc_memory_entry_t *entry)
{
    if (!entry) return;
    memset(entry, 0, sizeof(cc_memory_entry_t));
}

/**
 * @brief 释放一条记忆拥有的四个字符串，并将条目恢复为空状态。
 * @pre 条目已初始化，非空字符串可分别由 free 释放且互不共享拥有权，所有借用者已退出。
 * @param[in,out] entry 借用的条目地址，允许 NULL，结构存储可为栈对象或数组元素。
 * @post 非空条目的字符串已释放且全部字段清零，结构存储继续归调用者，空状态可重复清理。
 */
void cc_memory_entry_free(cc_memory_entry_t *entry)
{
    if (!entry) return;
    free(entry->key);
    free(entry->value);
    free(entry->category);
    free(entry->session_id);
    memset(entry, 0, sizeof(cc_memory_entry_t));
}

/**
 * @brief 逐项清理查询结果字段，再释放拥有的记忆数组存储。
 * @pre 非空数组可由 free 释放，前 count 项均已初始化且拥有字段不别名，全部借用者已退出。
 * @param[in] entries 消费拥有的堆数组，允许 NULL，此时直接返回。
 * @param[in] count 待清理的有效条目数量，由调用者保证不超过实际数组长度。
 * @post 非空数组及指定条目的字段已释放，调用者须清空自己保存的悬空数组指针。
 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count)
{
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        cc_memory_entry_free(&entries[i]);
    }
    free(entries);
}

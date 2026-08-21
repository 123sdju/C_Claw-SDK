



#include "cc/core/cc_memory_entry.h"
#include <stdlib.h>
#include <string.h>

/*
 * 初始化 memory entry。
 *
 * 适配器常在栈上临时构造 entry，再填充字符串字段；先清零可以保证后续失败路径
 * 调用 cc_memory_entry_free() 时不会释放未初始化指针。
 */
void cc_memory_entry_init(cc_memory_entry_t *entry)
{
    if (!entry) return;
    memset(entry, 0, sizeof(cc_memory_entry_t));
}

/*
 * 释放单条 memory entry 的内部资源。
 *
 * entry 本身可能是数组元素或调用方栈对象，所以这里只释放拥有的字符串字段并清零，
 * 不释放 entry 指针。
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

/*
 * 释放 memory entry 数组。
 *
 * memory store 查询通常返回堆数组；这个 helper 先逐项释放字段，再释放数组缓冲，
 * 让调用方不用重复写循环清理逻辑。
 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count)
{
    if (!entries) return;
    for (size_t i = 0; i < count; i++) {
        cc_memory_entry_free(&entries[i]);
    }
    free(entries);
}

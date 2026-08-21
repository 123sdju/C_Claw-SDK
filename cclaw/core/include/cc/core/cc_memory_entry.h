



#ifndef CC_MEMORY_ENTRY_H
#define CC_MEMORY_ENTRY_H

#include <stddef.h>
#include <time.h>

/*
 * memory store 中的一条键值记忆。
 *
 * key/value/category/session_id 都由 entry 拥有；adapter 从数据库或文件读出后必须
 * 分配独立字符串，调用方通过 cc_memory_entry_free() 释放。created_at/updated_at
 * 使用 time_t，便于 POSIX 和 MCU port 选择自己的时间来源。
 */
typedef struct cc_memory_entry {
    char *key;

    char *value;

    char *category;

    char *session_id;

    time_t created_at;

    time_t updated_at;

} cc_memory_entry_t;

/* 初始化 entry 为可释放的空状态；适合栈对象和数组元素。 */
void cc_memory_entry_init(cc_memory_entry_t *entry);

/* 释放 entry 拥有的字符串并清零；不释放 entry 指针本身。 */
void cc_memory_entry_free(cc_memory_entry_t *entry);

/* 释放由 memory store 返回的 entry 数组和其中每个元素。 */
void cc_memory_entry_free_array(cc_memory_entry_t *entries, size_t count);

#endif

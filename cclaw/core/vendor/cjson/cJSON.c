/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

/** @file 维护 cJSON 1.7.19 的节点、解析、打印与链表操作，许可原文保留于文件开头。
 * @note 独立树可并行操作，同一树的读写和销毁由调用者同步。
 * 分配钩子为进程共享配置，切换前须关闭入口并释放仍依赖旧钩子的节点与输出。
 * 数字转换和大小写比较依赖稳定的进程 locale，调用期间禁止并发 setlocale。
 * 引用父节点的借用 child 链只读，结构操作失败并保留链和新节点的原拥有权。
 */

/* 在 MSVC 关闭旧 C89 标准函数的弃用提示。 */
#if !defined(_CRT_SECURE_NO_DEPRECATE) && defined(_MSC_VER)
#define _CRT_SECURE_NO_DEPRECATE
#endif

#ifdef __GNUC__
#pragma GCC visibility push(default)
#endif
#if defined(_MSC_VER)
#pragma warning (push)
/* 系统头文件中的单行注释按平台头原样接受。 */
#pragma warning (disable : 4001)
#endif

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <limits.h>
#include <ctype.h>
#include <float.h>

#ifdef ENABLE_LOCALES
#include <locale.h>
#endif

#if defined(_MSC_VER)
#pragma warning (pop)
#endif
#ifdef __GNUC__
#pragma GCC visibility pop
#endif

#include "cJSON.h"

/* 使用本文件的 cJSON_bool 常量，避免外部宏改变其类型。 */
#ifdef true
#undef true
#endif
#define true ((cJSON_bool)1)

#ifdef false
#undef false
#endif
#define false ((cJSON_bool)0)

/* 对未由 math.h 提供分类宏的编译器补充 NaN/无穷判断。 */
#ifndef isinf
#define isinf(d) (isnan((d - d)) && !isnan(d))
#endif
#ifndef isnan
#define isnan(d) (d != d)
#endif

#ifndef NAN
#ifdef _WIN32
#define NAN sqrt(-1.0)
#else
#define NAN 0.0/0.0
#endif
#endif

typedef struct {
    const unsigned char *json; /* 借用最近解析输入，调用者负责其寿命。 */
    size_t position; /* 输入内的错误字节偏移。 */
} error;
/* 错误槽随线程/任务隔离，不支持 TLS 的编译器直接拒绝构建。 */
#if defined(__cplusplus) && (__cplusplus >= 201103L)
#define CJSON_THREAD_LOCAL thread_local
#elif defined(_MSC_VER)
#define CJSON_THREAD_LOCAL __declspec(thread)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define CJSON_THREAD_LOCAL _Thread_local
#elif defined(__GNUC__) || defined(__clang__)
#define CJSON_THREAD_LOCAL __thread
#else
#error cJSON requires thread-local storage for independent parse errors
#endif
static CJSON_THREAD_LOCAL error global_error = { NULL, 0 };

/** @brief 读取本线程错误槽，空槽直接返回 NULL，避免空指针算术。
 * @return 最近输入中的借用错误位置，输入释放后该借用失效。
 */
CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void)
{
    return global_error.json ? (const char*) (global_error.json + global_error.position) : NULL;
}

/** @brief 仅从 String 类型节点取得文本。
 * @param[in] item 稳定借用节点，可为 NULL。
 * @return valuestring 借用，类型不匹配或缺内容返回 NULL。
 */
CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item)
{
    if (!cJSON_IsString(item))
    {
        return NULL;
    }

    return item->valuestring;
}

/** @brief 检查类型后读取节点 double。
 * @param[in] item 稳定借用节点，可为 NULL。
 * @return Number 的值，其余情况返回 NaN。
 */
CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item)
{
    if (!cJSON_IsNumber(item))
    {
        return (double) NAN;
    }

    return item->valuedouble;
}

/* 拒绝将该实现与版本不同的头文件混合编译。 */
#if (CJSON_VERSION_MAJOR != 1) || (CJSON_VERSION_MINOR != 7) || (CJSON_VERSION_PATCH != 19)
    #error cJSON.h and cJSON.c have different versions. Make sure that both have the same.
#endif

/** @brief 返回常量版本文本，消除共享可写缓冲的重复格式化。
 * @return 永久有效的只读借用，版本与上方编译期检查对应。
 */
CJSON_PUBLIC(const char*) cJSON_Version(void)
{
    static const char version[] = "1.7.19";

    return version;
}

/** @brief 按当前 C locale 逐字节比较忽略大小写的键。
 * @param[in] string1 NUL 文本或 NULL。
 * @param[in] string2 NUL 文本或 NULL。
 * @return 相等为零，否则为非零；任一 NULL 均不等，包括两个 NULL。
 */
static int case_insensitive_strcmp(const unsigned char *string1, const unsigned char *string2)
{
    if ((string1 == NULL) || (string2 == NULL))
    {
        return 1;
    }

    if (string1 == string2)
    {
        return 0;
    }

    for(; tolower(*string1) == tolower(*string2); (void)string1++, string2++)
    {
        if (*string1 == '\0')
        {
            return 0;
        }
    }

    return tolower(*string1) - tolower(*string2);
}

typedef struct internal_hooks
{
    /** @brief 申请字节块。
     * @param[in] size 字节数，可能为零。
     * @return 独占对齐内存或 NULL。
     */
    void *(CJSON_CDECL *allocate)(size_t size);
    /** @brief 归还配对分配。
     * @param[in] pointer 独占地址或 NULL。
     * @post 分配已归还。
     */
    void (CJSON_CDECL *deallocate)(void *pointer);
    /** @brief 调整独占块容量，仅标准钩子时提供。
     * @param[in,out] pointer 原分配地址，成功后旧地址失效。
     * @param[in] size 新字节数。
     * @return 新独占地址，失败 NULL 并保留原块。
     */
    void *(CJSON_CDECL *reallocate)(void *pointer, size_t size);
} internal_hooks;

#if defined(_MSC_VER)
/* MSVC 需要本地包装以初始化引用导入 CRT 函数的静态钩子。 */
/** @brief MSVC 标准分配包装。
 * @param[in] size 请求字节数。
 * @return malloc 的独占分配或 NULL。
 */
static void * CJSON_CDECL internal_malloc(size_t size)
{
    return malloc(size);
}
/** @brief MSVC 标准释放包装。
 * @param[in] pointer 配对地址或 NULL。
 * @post 标准分配已归还。
 */
static void CJSON_CDECL internal_free(void *pointer)
{
    free(pointer);
}
/** @brief MSVC 标准重分配包装。
 * @param[in,out] pointer 旧独占块或 NULL，失败仍有效。
 * @param[in] size 新容量字节数，零沿 CRT 语义。
 * @return CRT 重分配结果。
 */
static void * CJSON_CDECL internal_realloc(void *pointer, size_t size)
{
    return realloc(pointer, size);
}
#else
#define internal_malloc malloc
#define internal_free free
#define internal_realloc realloc
#endif

/* 字符串字面量在编译期扣除尾 NUL，参数须为数组字面量。 */
#define static_strlen(string_literal) (sizeof(string_literal) - sizeof(""))

/* 全局钩子在开放入口前固定，单次解析/打印复制其快照。 */
static internal_hooks global_hooks = { internal_malloc, internal_free, internal_realloc };

/** @brief 用指定钩子复制包含 NUL 的完整 C 文本。
 * @param[in] string 借用文本，NULL 失败。
 * @param[in] hooks 有效且稳定的配对分配钩子。
 * @return 独占副本或 NULL，输入保持原样。
 */
static unsigned char* cJSON_strdup(const unsigned char* string, const internal_hooks * const hooks)
{
    size_t length = 0;
    unsigned char *copy = NULL;

    if (string == NULL)
    {
        return NULL;
    }

    length = strlen((const char*)string) + sizeof("");
    copy = (unsigned char*)hooks->allocate(length);
    if (copy == NULL)
    {
        return NULL;
    }
    memcpy(copy, string, length);

    return copy;
}

/** @brief 更新共享分配函数，按配对关系决定能否使用 realloc。
 * @param[in] hooks 可选借用表，NULL 恢复 malloc/free/realloc。
 * @pre 所有调用已排空，依赖原钩子的树和输出已释放。
 * @post 钩子函数已复制，调用者可释放表，缺省函数采用标准 CRT。
 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks)
{
    if (hooks == NULL)
    {
        /* 恢复标准分配器配对。 */
        global_hooks.allocate = malloc;
        global_hooks.deallocate = free;
        global_hooks.reallocate = realloc;
        return;
    }

    global_hooks.allocate = malloc;
    if (hooks->malloc_fn != NULL)
    {
        global_hooks.allocate = hooks->malloc_fn;
    }

    global_hooks.deallocate = free;
    if (hooks->free_fn != NULL)
    {
        global_hooks.deallocate = hooks->free_fn;
    }

    /* 只有标准 malloc/free 的块才能交给标准 realloc。 */
    global_hooks.reallocate = NULL;
    if ((global_hooks.allocate == malloc) && (global_hooks.deallocate == free))
    {
        global_hooks.reallocate = realloc;
    }
}

/** @brief 申请并零初始化一个尚未挂接的节点。
 * @param[in] hooks 稳定配对钩子，不能为空。
 * @return 独占零节点，分配失败 NULL。
 */
static cJSON *cJSON_New_Item(const internal_hooks * const hooks)
{
    cJSON* node = (cJSON*)hooks->allocate(sizeof(cJSON));
    if (node)
    {
        memset(node, '\0', sizeof(cJSON));
    }

    return node;
}

/** @brief 迭代归还同级链，对拥有的 child 递归清理。
 * @param[in] item 独占链首或 NULL，链及拥有子树须无环。
 * @post 节点本体、拥有值串与非常量键已归还；引用资源保留。
 * @note 用当前共享释放器，调用期间钩子及树须稳定，栈随 child 深度增长。
 */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item)
{
    cJSON *next = NULL;
    while (item != NULL)
    {
        next = item->next;
        if (!(item->type & cJSON_IsReference) && (item->child != NULL))
        {
            cJSON_Delete(item->child);
        }
        if (!(item->type & cJSON_IsReference) && (item->valuestring != NULL))
        {
            global_hooks.deallocate(item->valuestring);
            item->valuestring = NULL;
        }
        if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
        {
            global_hooks.deallocate(item->string);
            item->string = NULL;
        }
        global_hooks.deallocate(item);
        item = next;
    }
}

/** @brief 取得数字转换所用的小数字节。
 * @return ENABLE_LOCALES 时读取 locale 小数点首字节，其余构建返回点号。
 * @pre 使用 locale 的构建不得与进程 setlocale 并发。
 */
static unsigned char get_decimal_point(void)
{
#ifdef ENABLE_LOCALES
    struct lconv *lconv = localeconv();
    return (unsigned char) lconv->decimal_point[0];
#else
    return '.';
#endif
}

typedef struct
{
    const unsigned char *content; /* 借用显式长度输入。 */
    size_t length; /* 可读总字节数，可能包含尾 NUL。 */
    size_t offset; /* 当前读取字节偏移。 */
    size_t depth; /* 已进入的数组/对象容器层数。 */
    internal_hooks hooks; /* 本次解析开始时复制的分配器。 */
} parse_buffer;

/* 从当前偏移检查剩余可读字节数。 */
#define can_read(buffer, size) ((buffer != NULL) && (((buffer)->offset + size) <= (buffer)->length))
/* 检查相对当前偏移的零基字节位置。 */
#define can_access_at_index(buffer, index) ((buffer != NULL) && (((buffer)->offset + index) < (buffer)->length))
#define cannot_access_at_index(buffer, index) (!can_access_at_index(buffer, index))
/* 在前置范围检查后取得当前输入地址。 */
#define buffer_at_offset(buffer) ((buffer)->content + (buffer)->offset)

/** @brief 复制候选数字供 strtod 消费，并提交实际消耗的前缀。
 * @param[in,out] item 新建零节点，成功写入 Number/双精度值/饱和整数缓存。
 * @param[in,out] input_buffer 稳定输入与本次钩子，成功推进 offset。
 * @return 成功真；分配失败或无数字前缀为假，临时文本总会释放。
 * @note 保留 strtod 的前缀数字语义，不额外执行 JSON 数字文法检查。
 */
static cJSON_bool parse_number(cJSON * const item, parse_buffer * const input_buffer)
{
    double number = 0;
    unsigned char *after_end = NULL;
    unsigned char *number_c_string;
    unsigned char decimal_point = get_decimal_point();
    size_t i = 0;
    size_t number_string_length = 0;
    cJSON_bool has_decimal_point = false;

    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false;
    }

    /* 只扫描范围内数字候选字符，再复制并补 NUL 供 strtod 使用。 */
    for (i = 0; can_access_at_index(input_buffer, i); i++)
    {
        switch (buffer_at_offset(input_buffer)[i])
        {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
            case '+':
            case '-':
            case 'e':
            case 'E':
                number_string_length++;
                break;

            case '.':
                number_string_length++;
                has_decimal_point = true;
                break;

            default:
                goto loop_end;
        }
    }
loop_end:
    /* 候选副本额外保留一个 NUL 字节。 */
    number_c_string = (unsigned char *) input_buffer->hooks.allocate(number_string_length + 1);
    if (number_c_string == NULL)
    {
        return false; /* 候选缓冲分配失败。 */
    }

    memcpy(number_c_string, buffer_at_offset(input_buffer), number_string_length);
    number_c_string[number_string_length] = '\0';

    if (has_decimal_point)
    {
        for (i = 0; i < number_string_length; i++)
        {
            if (number_c_string[i] == '.')
            {
                /* strtod 使用当前 locale 的小数点。 */
                number_c_string[i] = decimal_point;
            }
        }
    }

    number = strtod((const char*)number_c_string, (char**)&after_end);
    if (number_c_string == after_end)
    {
        /* 没有数字前缀，归还候选副本。 */
        input_buffer->hooks.deallocate(number_c_string);
        return false; /* 数字解析失败。 */
    }

    item->valuedouble = number;

    /* int 缓存越界时按两端饱和。 */
    if (number >= INT_MAX)
    {
        item->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        item->valueint = INT_MIN;
    }
    else
    {
        item->valueint = (int)number;
    }

    item->type = cJSON_Number;

    input_buffer->offset += (size_t)(after_end - number_c_string);
    /* 只推进 strtod 实际消费的字节并归还副本。 */
    input_buffer->hooks.deallocate(number_c_string);
    return true;
}

/** @brief 同步更新双精度值与 int 缓存，NaN 避免浮点到整数的未定义转换。
 * @param[in,out] object 可修改节点，NULL 不写入。
 * @param[in] number 新数字，NaN 缓存零，超界缓存饱和。
 * @return 写入的 double；object 为 NULL 返回 NaN。
 */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number)
{
    if (object == NULL)
    {
        return (double)NAN;
    }

    if (isnan(number))
    {
        object->valueint = 0;
    }
    else if (number >= INT_MAX)
    {
        object->valueint = INT_MAX;
    }
    else if (number <= (double)INT_MIN)
    {
        object->valueint = INT_MIN;
    }
    else
    {
        object->valueint = (int)number;
    }

    return object->valuedouble = number;
}

/** @brief 更新拥有型 String，先完成可能失败的复制再释放旧内容。
 * @param[in,out] object 可修改拥有节点，NULL/引用/缺少旧文本均失败。
 * @param[in] valuestring 借用 C 文本，可为旧值或内部子串，NULL 失败。
 * @return 成功返回新文本借用，失败 NULL 且原值保持。
 */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring)
{
    char *copy = NULL;
    size_t v1_len;
    size_t v2_len;
    /* 引用文本仍由外部 owner 管理，只修改拥有型 String。 */
    if ((object == NULL) || !(object->type & cJSON_String) || (object->type & cJSON_IsReference))
    {
        return NULL;
    }
    /* 没有旧缓冲或新文本时保留节点。 */
    if (object->valuestring == NULL || valuestring == NULL)
    {
        return NULL;
    }

    v1_len = strlen(valuestring);
    v2_len = strlen(object->valuestring);

    if (v1_len <= v2_len)
    {
        /* 保留已有容量，memmove 同时处理自身或内部子串借用。 */
        memmove(object->valuestring, valuestring, v1_len + 1);
        return object->valuestring;
    }
    copy = (char*) cJSON_strdup((const unsigned char*)valuestring, &global_hooks);
    if (copy == NULL)
    {
        return NULL;
    }
    if (object->valuestring != NULL)
    {
        cJSON_free(object->valuestring);
    }
    object->valuestring = copy;

    return copy;
}

typedef struct
{
    unsigned char *buffer; /* 动态模式独占，noalloc 模式借用调用者缓冲。 */
    size_t length; /* 总容量字节数，包含可用终止空间。 */
    size_t offset; /* 当前片段起点或已合入的文本长度。 */
    size_t depth; /* 当前容器层数，同时用于缩进与深度上限。 */
    cJSON_bool noalloc; /* 调用者缓冲模式禁止扩容与释放。 */
    cJSON_bool format; /* 是否输出换行和缩进。 */
    internal_hooks hooks; /* 本次打印的稳定钩子快照。 */
} printbuffer;

/** @brief 保证当前写位置后容纳 needed 与终止空间，按需扩容。
 * @param[in,out] p 打印缓冲，失败扩容会归还拥有块并置空；预分配模式保留块。
 * @param[in] needed 本次额外需求，超过 INT_MAX 失败。
 * @return 当前可写地址或 NULL，可能使以前取得的缓冲借用失效。
 */
static unsigned char* ensure(printbuffer * const p, size_t needed)
{
    unsigned char *newbuffer = NULL;
    size_t newsize = 0;

    if ((p == NULL) || (p->buffer == NULL))
    {
        return NULL;
    }

    if ((p->length > 0) && (p->offset >= p->length))
    {
        /* 非空缓冲的偏移必须落在实际容量内。 */
        return NULL;
    }

    if (needed > INT_MAX)
    {
        /* 当前扩容策略只支持 int 范围内的容量。 */
        return NULL;
    }

    needed += p->offset + 1;
    if (needed <= p->length)
    {
        return p->buffer + p->offset;
    }

    if (p->noalloc) {
        return NULL;
    }

    /* 翻倍增长，并在接近 int 上限时封顶。 */
    if (needed > (INT_MAX / 2))
    {
        /* 避免倍增溢出，仍超出上限则失败。 */
        if (needed <= INT_MAX)
        {
            newsize = INT_MAX;
        }
        else
        {
            return NULL;
        }
    }
    else
    {
        newsize = needed * 2;
    }

    if (p->hooks.reallocate != NULL)
    {
        /* 标准钩子可直接重新分配，失败按打印失败撤销拥有块。 */
        newbuffer = (unsigned char*)p->hooks.reallocate(p->buffer, newsize);
        if (newbuffer == NULL)
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }
    }
    else
    {
        /* 自定义钩子采用申请、复制、释放，零容量旧块没有可读字节。 */
        newbuffer = (unsigned char*)p->hooks.allocate(newsize);
        if (!newbuffer)
        {
            p->hooks.deallocate(p->buffer);
            p->length = 0;
            p->buffer = NULL;

            return NULL;
        }

        if (p->length != 0)
        {
            memcpy(newbuffer, p->buffer, p->offset + 1);
        }
        p->hooks.deallocate(p->buffer);
    }
    p->length = newsize;
    p->buffer = newbuffer;

    return newbuffer + p->offset;
}

/** @brief 将当前写位置之后的已终止片段长度合入 offset。
 * @param[in,out] buffer 打印缓冲，NULL/空块不操作；当前片段须有 NUL。
 * @post offset 推进至片段尾 NUL，不重新分配或释放。
 */
static void update_offset(printbuffer * const buffer)
{
    const unsigned char *buffer_pointer = NULL;
    if ((buffer == NULL) || (buffer->buffer == NULL))
    {
        return;
    }
    buffer_pointer = buffer->buffer + buffer->offset;

    buffer->offset += strlen((const char*)buffer_pointer);
}

/** @brief 按最大绝对值乘 DBL_EPSILON 比较浮点近似相等。
 * @param[in] a 第一个双精度值。
 * @param[in] b 第二个双精度值。
 * @return 满足相对误差返回真，NaN 等不满足情形返回假。
 */
static cJSON_bool compare_double(double a, double b)
{
    double maxVal = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
    return (fabs(a - b) <= maxVal * DBL_EPSILON);
}

/** @brief 在局部数字缓冲中选择精度，再追加到打印缓冲。
 * @param[in] item 稳定 Number 节点。
 * @param[in,out] output_buffer 打印状态，NULL 失败，成功推进 offset。
 * @return 成功真，格式化/容量失败假；NaN/无穷输出 null。
 */
static cJSON_bool print_number(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    double d = item->valuedouble;
    int length = 0;
    size_t i = 0;
    unsigned char number_buffer[26] = {0}; /* double 的局部格式化片段。 */
    unsigned char decimal_point = get_decimal_point();
    double test = 0.0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* JSON 没有 NaN/无穷字面量。 */
    if (isnan(d) || isinf(d))
    {
        length = snprintf((char*)number_buffer, sizeof(number_buffer), "null");
    }
    else if(d == (double)item->valueint)
    {
        length = snprintf((char*)number_buffer, sizeof(number_buffer), "%d", item->valueint);
    }
    else
    {
        /* 先用十五位有效数字减少无意义尾数。 */
        length = snprintf((char*)number_buffer, sizeof(number_buffer), "%1.15g", d);
        if ((length < 0) || ((size_t)length >= sizeof(number_buffer)))
        {
            return false;
        }

        /* 回读检查能否恢复原 double。 */
        if ((sscanf((char*)number_buffer, "%lg", &test) != 1) || !compare_double((double)test, d))
        {
            /* 精度不足时改用十七位有效数字。 */
            length = snprintf((char*)number_buffer, sizeof(number_buffer), "%1.17g", d);
        }
    }

    /* 不把失败长度或超出局部容量的长度交给后续复制。 */
    if ((length < 0) || (length > (int)(sizeof(number_buffer) - 1)))
    {
        return false;
    }

    /* 预留数字片段及终止空间。 */
    output_pointer = ensure(output_buffer, (size_t)length + sizeof(""));
    if (output_pointer == NULL)
    {
        return false;
    }

    /* 输出 JSON 固定点号，替换 locale 转换使用的小数点。 */
    for (i = 0; i < ((size_t)length); i++)
    {
        if (number_buffer[i] == decimal_point)
        {
            output_pointer[i] = '.';
            continue;
        }

        output_pointer[i] = number_buffer[i];
    }
    output_pointer[i] = '\0';

    output_buffer->offset += (size_t)length;

    return true;
}

/** @brief 读取恰好四个十六进制字节，区分合法零与非法字符。
 * @param[in] input 至少四个可读字节，调用者已验证范围。
 * @return 0..65535 的码元，非法字符返回 UINT_MAX。
 */
static unsigned parse_hex4(const unsigned char * const input)
{
    unsigned int h = 0;
    size_t i = 0;

    for (i = 0; i < 4; i++)
    {
        /* 累加一个十六进制数位。 */
        if ((input[i] >= '0') && (input[i] <= '9'))
        {
            h += (unsigned int) input[i] - '0';
        }
        else if ((input[i] >= 'A') && (input[i] <= 'F'))
        {
            h += (unsigned int) 10 + input[i] - 'A';
        }
        else if ((input[i] >= 'a') && (input[i] <= 'f'))
        {
            h += (unsigned int) 10 + input[i] - 'a';
        }
        else /* 非法十六进制字节。 */
        {
            return UINT_MAX;
        }

        if (i < 3)
        {
            /* 为后续四位数值腾出空间。 */
            h = h << 4;
        }
    }

    return h;
}

/** @brief 将 Unicode 单转义或代理对转义转换为 UTF-8。
 * @param[in] input_pointer 指向反斜杠 u 的借用输入。
 * @param[in] input_end 字符串结束引号地址，读取不得越过该边界。
 * @param[in,out] output_pointer 目标游标，容量已由上层保证，成功推进一至四字节。
 * @return 成功消费六或十二字节，坏十六进制/代理配对/范围返回零。
 * @note 合法 u0000 写入 NUL，上层 C 文本接口可能只暴露其前缀。
 */
static unsigned char utf16_literal_to_utf8(const unsigned char * const input_pointer, const unsigned char * const input_end, unsigned char **output_pointer)
{
    long unsigned int codepoint = 0;
    unsigned int first_code = 0;
    const unsigned char *first_sequence = input_pointer;
    unsigned char utf8_length = 0;
    unsigned char utf8_position = 0;
    unsigned char sequence_length = 0;
    unsigned char first_byte_mark = 0;

    if ((input_end - first_sequence) < 6)
    {
        /* 剩余字节不足以容纳一个转义。 */
        goto fail;
    }

    /* 解析第一个 UTF-16 码元。 */
    first_code = parse_hex4(first_sequence + 2);

    /* 拒绝非法十六进制与独立低代理。 */
    if ((first_code == UINT_MAX) || ((first_code >= 0xDC00) && (first_code <= 0xDFFF)))
    {
        goto fail;
    }

    /* 高代理必须跟随合法低代理。 */
    if ((first_code >= 0xD800) && (first_code <= 0xDBFF))
    {
        const unsigned char *second_sequence = first_sequence + 6;
        unsigned int second_code = 0;
        sequence_length = 12; /* 代理对转义共十二字节。 */

        if ((input_end - second_sequence) < 6)
        {
            /* 剩余字节不足以容纳低代理转义。 */
            goto fail;
        }

        if ((second_sequence[0] != '\\') || (second_sequence[1] != 'u'))
        {
            /* 缺少低代理转义前缀。 */
            goto fail;
        }

        /* 解析第二个码元，非法哨兵也由后续范围检查拒绝。 */
        second_code = parse_hex4(second_sequence + 2);
        /* 第二个码元必须在低代理范围。 */
        if ((second_code < 0xDC00) || (second_code > 0xDFFF))
        {
            /* 低代理无效。 */
            goto fail;
        }


        /* 从代理对合成完整码点。 */
        codepoint = 0x10000 + (((first_code & 0x3FF) << 10) | (second_code & 0x3FF));
    }
    else
    {
        sequence_length = 6; /* 单转义共六字节。 */
        codepoint = first_code;
    }

    /* 根据码点范围选择一至四字节 UTF-8。 */
    if (codepoint < 0x80)
    {
        /* ASCII 使用单字节。 */
        utf8_length = 1;
    }
    else if (codepoint < 0x800)
    {
        /* 两字节编码。 */
        utf8_length = 2;
        first_byte_mark = 0xC0; /* 两字节首标记。 */
    }
    else if (codepoint < 0x10000)
    {
        /* 三字节编码。 */
        utf8_length = 3;
        first_byte_mark = 0xE0; /* 三字节首标记。 */
    }
    else if (codepoint <= 0x10FFFF)
    {
        /* 四字节编码。 */
        utf8_length = 4;
        first_byte_mark = 0xF0; /* 四字节首标记。 */
    }
    else
    {
        /* 超出合法码点范围。 */
        goto fail;
    }

    /* 从末个续字节向前写入。 */
    for (utf8_position = (unsigned char)(utf8_length - 1); utf8_position > 0; utf8_position--)
    {
        /* 续字节最高两位为 10。 */
        (*output_pointer)[utf8_position] = (unsigned char)((codepoint | 0x80) & 0xBF);
        codepoint >>= 6;
    }
    /* 合成首字节。 */
    if (utf8_length > 1)
    {
        (*output_pointer)[0] = (unsigned char)((codepoint | first_byte_mark) & 0xFF);
    }
    else
    {
        (*output_pointer)[0] = (unsigned char)(codepoint & 0x7F);
    }

    *output_pointer += utf8_length;

    return sequence_length;

fail:
    return 0;
}

/** @brief 查找结束引号、分配容量上界并解码转义。
 * @param[in,out] item 新建节点，成功接管独占文本并标记 String。
 * @param[in,out] input_buffer 已指向范围内引号，成功推进至结束引号后。
 * @return 成功真，转义/边界/OOM 失败假并清理文本，offset 留在错误附近。
 * @note 普通字节原样复制，不独立验证 UTF-8 或收紧所有控制字节语法。
 */
static cJSON_bool parse_string(cJSON * const item, parse_buffer * const input_buffer)
{
    const unsigned char *input_pointer = buffer_at_offset(input_buffer) + 1;
    const unsigned char *input_end = buffer_at_offset(input_buffer) + 1;
    unsigned char *output_pointer = NULL;
    unsigned char *output = NULL;

    /* 入口必须指向首引号。 */
    if (buffer_at_offset(input_buffer)[0] != '\"')
    {
        goto fail;
    }

    {
        /* 跳过转义前缀，计算解码目标的容量上界。 */
        size_t allocation_length = 0;
        size_t skipped_bytes = 0;
        while (((size_t)(input_end - input_buffer->content) < input_buffer->length) && (*input_end != '\"'))
        {
            /* 扫描时越过转义后的一个字节。 */
            if (input_end[0] == '\\')
            {
                if ((size_t)(input_end + 1 - input_buffer->content) >= input_buffer->length)
                {
                    /* 末字节为反斜杠时不得访问其后。 */
                    goto fail;
                }
                skipped_bytes++;
                input_end++;
            }
            input_end++;
        }
        if (((size_t)(input_end - input_buffer->content) >= input_buffer->length) || (*input_end != '\"'))
        {
            goto fail; /* 范围内缺少结束引号。 */
        }

        /* 解码长度不会超过这一上界，额外预留尾 NUL。 */
        allocation_length = (size_t) (input_end - buffer_at_offset(input_buffer)) - skipped_bytes;
        output = (unsigned char*)input_buffer->hooks.allocate(allocation_length + sizeof(""));
        if (output == NULL)
        {
            goto fail; /* 解码缓冲分配失败。 */
        }
    }

    output_pointer = output;
    /* 在已找到的字符串边界内解码。 */
    while (input_pointer < input_end)
    {
        if (*input_pointer != '\\')
        {
            *output_pointer++ = *input_pointer++;
        }
        /* 按转义类型写入解码字节。 */
        else
        {
            unsigned char sequence_length = 2;
            if ((input_end - input_pointer) < 1)
            {
                goto fail;
            }

            switch (input_pointer[1])
            {
                case 'b':
                    *output_pointer++ = '\b';
                    break;
                case 'f':
                    *output_pointer++ = '\f';
                    break;
                case 'n':
                    *output_pointer++ = '\n';
                    break;
                case 'r':
                    *output_pointer++ = '\r';
                    break;
                case 't':
                    *output_pointer++ = '\t';
                    break;
                case '\"':
                case '\\':
                case '/':
                    *output_pointer++ = input_pointer[1];
                    break;

                /* 单码元或代理对转义。 */
                case 'u':
                    sequence_length = utf16_literal_to_utf8(input_pointer, input_end, &output_pointer);
                    if (sequence_length == 0)
                    {
                        /* 转义形式或代理配对无效。 */
                        goto fail;
                    }
                    break;

                default:
                    goto fail;
            }
            input_pointer += sequence_length;
        }
    }

    /* 补尾 NUL 后交给节点。 */
    *output_pointer = '\0';

    item->type = cJSON_String;
    item->valuestring = (char*)output;

    input_buffer->offset = (size_t) (input_end - input_buffer->content);
    input_buffer->offset++;

    return true;

fail:
    if (output != NULL)
    {
        input_buffer->hooks.deallocate(output);
        output = NULL;
    }

    if (input_pointer != NULL)
    {
        input_buffer->offset = (size_t)(input_pointer - input_buffer->content);
    }

    return false;
}

/** @brief 计数转义增长并写入带引号的文本片段。
 * @param[in] input 稳定 C 文本，NULL 按空字符串输出。
 * @param[in,out] output_buffer 打印状态，成功补 NUL，offset 由上层合入。
 * @return 成功真，容量或分配失败假，可能撤销动态缓冲。
 */
static cJSON_bool print_string_ptr(const unsigned char * const input, printbuffer * const output_buffer)
{
    const unsigned char *input_pointer = NULL;
    unsigned char *output = NULL;
    unsigned char *output_pointer = NULL;
    size_t output_length = 0;
    /* 转义比原文额外增加的字节数。 */
    size_t escape_characters = 0;

    if (output_buffer == NULL)
    {
        return false;
    }

    /* NULL 文本保留空字符串兼容语义。 */
    if (input == NULL)
    {
        output = ensure(output_buffer, sizeof("\"\""));
        if (output == NULL)
        {
            return false;
        }
        strcpy((char*)output, "\"\"");

        return true;
    }

    /* 计数引号、反斜杠和控制字节的输出增长。 */
    for (input_pointer = input; *input_pointer; input_pointer++)
    {
        switch (*input_pointer)
        {
            case '\"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                /* 短转义多一个字节。 */
                escape_characters++;
                break;
            default:
                if (*input_pointer < 32)
                {
                    /* 其它控制字节转为六字节转义。 */
                    escape_characters += 5;
                }
                break;
        }
    }
    output_length = (size_t)(input_pointer - input) + escape_characters;

    output = ensure(output_buffer, output_length + sizeof("\"\""));
    if (output == NULL)
    {
        return false;
    }

    /* 无转义时直接在首尾包裹引号。 */
    if (escape_characters == 0)
    {
        output[0] = '\"';
        memcpy(output + 1, input, output_length);
        output[output_length + 1] = '\"';
        output[output_length + 2] = '\0';

        return true;
    }

    output[0] = '\"';
    output_pointer = output + 1;
    /* 一次容量保证后写入全部片段。 */
    for (input_pointer = input; *input_pointer != '\0'; (void)input_pointer++, output_pointer++)
    {
        if ((*input_pointer > 31) && (*input_pointer != '\"') && (*input_pointer != '\\'))
        {
            /* 普通字节原样复制。 */
            *output_pointer = *input_pointer;
        }
        else
        {
            /* 写入转义前缀和代号。 */
            *output_pointer++ = '\\';
            switch (*input_pointer)
            {
                case '\\':
                    *output_pointer = '\\';
                    break;
                case '\"':
                    *output_pointer = '\"';
                    break;
                case '\b':
                    *output_pointer = 'b';
                    break;
                case '\f':
                    *output_pointer = 'f';
                    break;
                case '\n':
                    *output_pointer = 'n';
                    break;
                case '\r':
                    *output_pointer = 'r';
                    break;
                case '\t':
                    *output_pointer = 't';
                    break;
                default:
                    /* 将控制码元输出为四位十六进制。 */
                    sprintf((char*)output_pointer, "u%04x", *input_pointer);
                    output_pointer += 4;
                    break;
            }
        }
    }
    output[output_length + 1] = '\"';
    output[output_length + 2] = '\0';

    return true;
}

/** @brief 将节点文本转交给带引号打印器。
 * @param[in] item 稳定 String 节点。
 * @param[in,out] p 打印状态，NULL 由下层拒绝。
 * @return 下层打印结果，节点与文本仍由原 owner 管理。
 */
static cJSON_bool print_string(const cJSON * const item, printbuffer * const p)
{
    return print_string_ptr((unsigned char*)item->valuestring, p);
}

/** @brief 解析一个值并转交其拥有内容，供容器递归调用。
 * @param[in,out] item 新建节点，失败由调用者清理。
 * @param[in,out] input_buffer 稳定输入及消费游标。
 * @return 成功真，语法/深度/分配失败假。
 */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer);
/** @brief 按基础类型打印一个稳定节点。
 * @param[in] item 借用节点，NULL 失败。
 * @param[in,out] output_buffer 打印缓冲，失败交最外层撤销。
 * @return 完整输出真，否则假。
 */
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer);
/** @brief 完整解析数组后提交成员链。
 * @param[in,out] item 新建父节点，成功接管成员。
 * @param[in,out] input_buffer 指向数组首符号的解析状态。
 * @return 成功真，失败假且清理未发布链。
 */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer);
/** @brief 递归打印数组并维护容器深度。
 * @param[in] item 稳定借用数组节点。
 * @param[in,out] output_buffer 打印缓冲，成功恢复进入前深度。
 * @return 完整输出真，否则假。
 */
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer);
/** @brief 完整解析对象键值后提交成员链。
 * @param[in,out] item 新建父节点，成功接管键和值。
 * @param[in,out] input_buffer 指向对象首符号的解析状态。
 * @return 成功真，失败假且撤销未发布链。
 */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer);
/** @brief 递归打印对象的键和值。
 * @param[in] item 稳定借用对象节点。
 * @param[in,out] output_buffer 打印缓冲，成功恢复进入前深度。
 * @return 完整输出真，深度/容量/成员失败假。
 */
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer);

/** @brief 跳过不大于 ASCII 空格的输入字节。
 * @param[in,out] buffer 解析状态，NULL/空 content 返回 NULL。
 * @return 原状态，耗尽范围时停在末个可读字节，保留宽松空白语义。
 */
static parse_buffer *buffer_skip_whitespace(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL))
    {
        return NULL;
    }

    if (cannot_access_at_index(buffer, 0))
    {
        return buffer;
    }

    while (can_access_at_index(buffer, 0) && (buffer_at_offset(buffer)[0] <= 32))
    {
       buffer->offset++;
    }

    if (buffer->offset == buffer->length)
    {
        buffer->offset--;
    }

    return buffer;
}

/** @brief 仅在输入起始处越过 UTF-8 BOM。
 * @param[in,out] buffer offset 为零的解析状态。
 * @return 原状态，参数无效或非零偏移返回 NULL，无 BOM 时不推进。
 */
static parse_buffer *skip_utf8_bom(parse_buffer * const buffer)
{
    if ((buffer == NULL) || (buffer->content == NULL) || (buffer->offset != 0))
    {
        return NULL;
    }

    if (can_access_at_index(buffer, 2) && (strncmp((const char*)buffer_at_offset(buffer), "\xEF\xBB\xBF", 3) == 0))
    {
        buffer->offset += 3;
    }

    return buffer;
}

/** @brief 计算含尾 NUL 的范围后委托解析。
 * @param[in] value 借用 C 文本，NULL 立即失败且不更新线程错误槽。
 * @param[out] return_parse_end 可选输入内借用位置，NULL 输入保持原槽。
 * @param[in] require_null_terminated 非零要求值后只剩空白与 NUL。
 * @return 独占树或 NULL，失败不交付部分树。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    size_t buffer_length;

    if (NULL == value)
    {
        return NULL;
    }

    /* 把尾 NUL 纳入可读范围供终止选项检查。 */
    buffer_length = strlen(value) + sizeof("");

    return cJSON_ParseWithLengthOpts(value, buffer_length, return_parse_end, require_null_terminated);
}

/** @brief 解析显式范围，集中归还失败现场并记录线程错误。
 * @param[in] value 借用可读输入，NULL 失败。
 * @param[in] buffer_length 可读总字节数，零失败。
 * @param[out] return_parse_end 可选借用位置，非 NULL 输入失败时写错误附近地址。
 * @param[in] require_null_terminated 非零检查值后空白与范围内 NUL。
 * @return 独占根节点或 NULL，失败释放部分树；NULL 输入保留结束槽。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated)
{
    parse_buffer buffer = { 0, 0, 0, 0, { 0, 0, 0 } };
    cJSON *item = NULL;

    /* 清空本线程旧错误位置。 */
    global_error.json = NULL;
    global_error.position = 0;

    if (value == NULL || 0 == buffer_length)
    {
        goto fail;
    }

    buffer.content = (const unsigned char*)value;
    buffer.length = buffer_length;
    buffer.offset = 0;
    buffer.hooks = global_hooks;

    item = cJSON_New_Item(&global_hooks);
    if (item == NULL) /* 根节点分配失败。 */
    {
        goto fail;
    }

    if (!parse_value(item, buffer_skip_whitespace(skip_utf8_bom(&buffer))))
    {
        /* 下层 offset 保留在错误附近。 */
        goto fail;
    }

    /* 尾部选项不额外收紧数字文法、控制字节与重复键。 */
    if (require_null_terminated)
    {
        buffer_skip_whitespace(&buffer);
        if ((buffer.offset >= buffer.length) || buffer_at_offset(&buffer)[0] != '\0')
        {
            goto fail;
        }
    }
    if (return_parse_end)
    {
        *return_parse_end = (const char*)buffer_at_offset(&buffer);
    }

    return item;

fail:
    if (item != NULL)
    {
        cJSON_Delete(item);
    }

    if (value != NULL)
    {
        error local_error;
        local_error.json = (const unsigned char*)value;
        local_error.position = 0;

        if (buffer.offset < buffer.length)
        {
            local_error.position = buffer.offset;
        }
        else if (buffer.length > 0)
        {
            local_error.position = buffer.length - 1;
        }

        if (return_parse_end != NULL)
        {
            *return_parse_end = (const char*)local_error.json + local_error.position;
        }

        global_error = local_error;
    }

    return NULL;
}

/** @brief 使用默认选项解析 C 文本的第一个值。
 * @param[in] value 借用 NUL 文本，NULL 失败。
 * @return 独占树或 NULL，允许剩余尾部。
 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value)
{
    return cJSON_ParseWithOpts(value, 0, 0);
}

/** @brief 使用默认选项解析字节范围中的第一个值。
 * @param[in] value 借用可读输入，NULL 失败。
 * @param[in] buffer_length 范围字节数，零失败。
 * @return 独占树或 NULL，失败清理部分结果。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length)
{
    return cJSON_ParseWithLengthOpts(value, buffer_length, 0, 0);
}

/* 取得两项较小值，参数可能重复求值，内部只传无副作用的长度。 */
#define cjson_min(a, b) (((a) < (b)) ? (a) : (b))

/** @brief 动态打印后收缩为包含 NUL 的精确拥有文本。
 * @param[in] item 稳定借用树，NULL 失败。
 * @param[in] format 是否输出缩进和换行。
 * @param[in] hooks 本次稳定配对钩子，不能为空。
 * @return 独占文本或 NULL，任意阶段失败归还全部拥有缓冲。
 */
static unsigned char *print(const cJSON * const item, cJSON_bool format, const internal_hooks * const hooks)
{
    static const size_t default_buffer_size = 256;
    printbuffer buffer[1];
    unsigned char *printed = NULL;

    memset(buffer, 0, sizeof(buffer));

    /* 以 256 字节动态缓冲起步。 */
    buffer->buffer = (unsigned char*) hooks->allocate(default_buffer_size);
    buffer->length = default_buffer_size;
    buffer->format = format;
    buffer->hooks = *hooks;
    if (buffer->buffer == NULL)
    {
        goto fail;
    }

    /* 完成树打印，再合入末片段长度。 */
    if (!print_value(item, buffer))
    {
        goto fail;
    }
    update_offset(buffer);

    /* 标准分配器直接收缩。 */
    if (hooks->reallocate != NULL)
    {
        printed = (unsigned char*) hooks->reallocate(buffer->buffer, buffer->offset + 1);
        if (printed == NULL) {
            goto fail;
        }
        buffer->buffer = NULL;
    }
    else /* 自定义钩子采用精确副本完成收缩。 */
    {
        printed = (unsigned char*) hooks->allocate(buffer->offset + 1);
        if (printed == NULL)
        {
            goto fail;
        }
        memcpy(printed, buffer->buffer, cjson_min(buffer->length, buffer->offset + 1));
        printed[buffer->offset] = '\0'; /* 明确补齐尾 NUL。 */

        /* 副本接管输出后归还增长缓冲。 */
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    return printed;

fail:
    if (buffer->buffer != NULL)
    {
        hooks->deallocate(buffer->buffer);
        buffer->buffer = NULL;
    }

    if (printed != NULL)
    {
        hooks->deallocate(printed);
        printed = NULL;
    }

    return NULL;
}

/** @brief 通过当前钩子格式化打印。
 * @param[in] item 稳定借用树，NULL 失败。
 * @return 独占文本或 NULL，失败无部分结果。
 */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item)
{
    return (char*)print(item, true, &global_hooks);
}

/** @brief 通过当前钩子紧凑打印。
 * @param[in] item 稳定借用树，NULL 失败。
 * @return 独占文本或 NULL。
 */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item)
{
    return (char*)print(item, false, &global_hooks);
}

/** @brief 按预计初始容量增长打印，成功保留最终块容量。
 * @param[in] item 稳定借用树，NULL 失败。
 * @param[in] prebuffer 初始字节数，负值失败，零沿分配器行为。
 * @param[in] fmt 非零格式化，零紧凑打印。
 * @return 独占 NUL 文本或 NULL，失败归还当前块。
 */
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if (prebuffer < 0)
    {
        return NULL;
    }

    p.buffer = (unsigned char*)global_hooks.allocate((size_t)prebuffer);
    if (!p.buffer)
    {
        return NULL;
    }

    p.length = (size_t)prebuffer;
    p.offset = 0;
    p.noalloc = false;
    p.format = fmt;
    p.hooks = global_hooks;

    if (!print_value(item, &p))
    {
        global_hooks.deallocate(p.buffer);
        p.buffer = NULL;
        return NULL;
    }

    return (char*)p.buffer;
}

/** @brief 禁止扩容，在调用者缓冲内打印。
 * @param[in] item 稳定借用树。
 * @param[out] buffer 调用者拥有的可写区域，失败可能保留部分片段。
 * @param[in] length 总容量字节数，负值失败。
 * @param[in] format 是否格式化。
 * @return 完整输出真，参数/深度/类型/容量失败假，不释放 buffer。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format)
{
    printbuffer p = { 0, 0, 0, 0, 0, 0, { 0, 0, 0 } };

    if ((length < 0) || (buffer == NULL))
    {
        return false;
    }

    p.buffer = (unsigned char*)buffer;
    p.length = (size_t)length;
    p.offset = 0;
    p.noalloc = true;
    p.format = format;
    p.hooks = global_hooks;

    return print_value(item, &p);
}

/** @brief 按首字节选择标量或容器解析器。
 * @param[in,out] item 新建零节点，成功接管解析内容。
 * @param[in,out] input_buffer 解析状态，成功推进 offset，失败位置供上层诊断。
 * @return 识别并解析成功真，否则假；节点失败清理由上层负责。
 */
static cJSON_bool parse_value(cJSON * const item, parse_buffer * const input_buffer)
{
    if ((input_buffer == NULL) || (input_buffer->content == NULL))
    {
        return false; /* 没有可读输入。 */
    }

    /* JSON null 字面量。 */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "null", 4) == 0))
    {
        item->type = cJSON_NULL;
        input_buffer->offset += 4;
        return true;
    }
    /* JSON false 字面量。 */
    if (can_read(input_buffer, 5) && (strncmp((const char*)buffer_at_offset(input_buffer), "false", 5) == 0))
    {
        item->type = cJSON_False;
        input_buffer->offset += 5;
        return true;
    }
    /* JSON true 字面量。 */
    if (can_read(input_buffer, 4) && (strncmp((const char*)buffer_at_offset(input_buffer), "true", 4) == 0))
    {
        item->type = cJSON_True;
        item->valueint = 1;
        input_buffer->offset += 4;
        return true;
    }
    /* 引号开始字符串。 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '\"'))
    {
        return parse_string(item, input_buffer);
    }
    /* 负号或数字开始数值。 */
    if (can_access_at_index(input_buffer, 0) && ((buffer_at_offset(input_buffer)[0] == '-') || ((buffer_at_offset(input_buffer)[0] >= '0') && (buffer_at_offset(input_buffer)[0] <= '9'))))
    {
        return parse_number(item, input_buffer);
    }
    /* 方括号开始数组。 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '['))
    {
        return parse_array(item, input_buffer);
    }
    /* 花括号开始对象。 */
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '{'))
    {
        return parse_object(item, input_buffer);
    }

    return false;
}

/** @brief 按基础类型分派打印，忽略拥有权高位标志。
 * @param[in] item 稳定借用节点，NULL 失败。
 * @param[in,out] output_buffer 打印状态，NULL 失败。
 * @return 成功真，类型/容量/深度失败假，拥有缓冲清理由最外层负责。
 */
static cJSON_bool print_value(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output = NULL;

    if ((item == NULL) || (output_buffer == NULL))
    {
        return false;
    }

    switch ((item->type) & 0xFF)
    {
        case cJSON_NULL:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "null");
            return true;

        case cJSON_False:
            output = ensure(output_buffer, 6);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "false");
            return true;

        case cJSON_True:
            output = ensure(output_buffer, 5);
            if (output == NULL)
            {
                return false;
            }
            strcpy((char*)output, "true");
            return true;

        case cJSON_Number:
            return print_number(item, output_buffer);

        case cJSON_Raw:
        {
            size_t raw_length = 0;
            if (item->valuestring == NULL)
            {
                return false;
            }

            raw_length = strlen(item->valuestring) + sizeof("");
            output = ensure(output_buffer, raw_length);
            if (output == NULL)
            {
                return false;
            }
            memcpy(output, item->valuestring, raw_length);
            return true;
        }

        case cJSON_String:
            return print_string(item, output_buffer);

        case cJSON_Array:
            return print_array(item, output_buffer);

        case cJSON_Object:
            return print_object(item, output_buffer);

        default:
            return false;
    }
}

/** @brief 解析数组成员链，完整结束后才交给父节点。
 * @param[in,out] item 新建父节点，成功接管独占 child 链。
 * @param[in,out] input_buffer 指向左方括号，成功推进至右方括号后。
 * @return 成功真；深度、语法或 OOM 失败假，释放未发布成员链。
 */
static cJSON_bool parse_array(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* 尚未发布的独占成员链首。 */
    cJSON *current_item = NULL;

    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* 容器深度已达上限。 */
    }
    input_buffer->depth++;

    if (buffer_at_offset(input_buffer)[0] != '[')
    {
        /* 入口不是数组首符号。 */
        goto fail;
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ']'))
    {
        /* 直接闭合为空数组。 */
        goto success;
    }

    /* 跳过空白后仍须有可读字节。 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;
        goto fail;
    }

    /* 循环统一先推进一个分隔符字节。 */
    input_buffer->offset--;
    /* 逐个解析逗号分隔成员。 */
    do
    {
        /* 先为下一个成员申请零节点。 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* 成员分配失败。 */
        }

        /* 仅挂入本层未发布链，失败可统一回收。 */
        if (head == NULL)
        {
            /* 第一个成员成为链首。 */
            current_item = head = new_item;
        }
        else
        {
            /* 链接新尾并推进。 */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        /* 跳过首分隔符或逗号后解析值。 */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* 成员值解析失败。 */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || buffer_at_offset(input_buffer)[0] != ']')
    {
        goto fail; /* 缺少数组结束符号。 */
    }

success:
    input_buffer->depth--;

    if (head != NULL) {
        head->prev = current_item;
    }

    item->type = cJSON_Array;
    item->child = head;

    input_buffer->offset++;

    return true;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false;
}

/** @brief 递归打印数组，按格式选项在逗号后加空格。
 * @param[in] item 稳定借用数组节点。
 * @param[in,out] output_buffer 打印状态，成功恢复进入前 depth。
 * @return 成功真，深度/容量/成员打印失败假，失败状态仅供外层撤销。
 */
static cJSON_bool print_array(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_element = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    if (output_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* 超过打印容器深度。 */
    }

    /* 写入数组左括号并计入当前层。 */
    output_pointer = ensure(output_buffer, 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer = '[';
    output_buffer->offset++;
    output_buffer->depth++;

    while (current_element != NULL)
    {
        if (!print_value(current_element, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);
        if (current_element->next)
        {
            length = (size_t) (output_buffer->format ? 2 : 1);
            output_pointer = ensure(output_buffer, length + 1);
            if (output_pointer == NULL)
            {
                return false;
            }
            *output_pointer++ = ',';
            if(output_buffer->format)
            {
                *output_pointer++ = ' ';
            }
            *output_pointer = '\0';
            output_buffer->offset += length;
        }
        current_element = current_element->next;
    }

    output_pointer = ensure(output_buffer, 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    *output_pointer++ = ']';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/** @brief 解析成员键和值，完整闭合后才提交对象 child 链。
 * @param[in,out] item 新建父节点，成功接管键和值的独占成员链。
 * @param[in,out] input_buffer 指向左花括号，成功推进至右花括号后。
 * @return 成功真，语法/深度/OOM 失败假，未发布链全部清理。
 */
static cJSON_bool parse_object(cJSON * const item, parse_buffer * const input_buffer)
{
    cJSON *head = NULL; /* 本层尚未发布的独占链首。 */
    cJSON *current_item = NULL;

    if (input_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* 超出容器深度上限。 */
    }
    input_buffer->depth++;

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '{'))
    {
        goto fail; /* 入口不是对象首符号。 */
    }

    input_buffer->offset++;
    buffer_skip_whitespace(input_buffer);
    if (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == '}'))
    {
        goto success; /* 直接闭合为空对象。 */
    }

    /* 跳过空白后仍需可读成员或结束符号。 */
    if (cannot_access_at_index(input_buffer, 0))
    {
        input_buffer->offset--;
        goto fail;
    }

    /* 让循环统一先跨过分隔符。 */
    input_buffer->offset--;
    /* 逐个处理逗号分隔的对象成员。 */
    do
    {
        /* 为下一组键值申请零节点。 */
        cJSON *new_item = cJSON_New_Item(&(input_buffer->hooks));
        if (new_item == NULL)
        {
            goto fail; /* 成员节点分配失败。 */
        }

        /* 只挂入本层临时链，尚不发布给父节点。 */
        if (head == NULL)
        {
            /* 保存链首。 */
            current_item = head = new_item;
        }
        else
        {
            /* 更新临时链尾。 */
            current_item->next = new_item;
            new_item->prev = current_item;
            current_item = new_item;
        }

        if (cannot_access_at_index(input_buffer, 1))
        {
            goto fail; /* 逗号后缺少可读成员。 */
        }

        /* 键先经字符串解析器解码。 */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_string(current_item, input_buffer))
        {
            goto fail; /* 成员键解析失败。 */
        }
        buffer_skip_whitespace(input_buffer);

        /* 把键的独占解码内容从 valuestring 移交 string。 */
        current_item->string = current_item->valuestring;
        current_item->valuestring = NULL;

        if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != ':'))
        {
            goto fail; /* 键后缺少冒号。 */
        }

        /* 越过冒号后复用成员节点解析值。 */
        input_buffer->offset++;
        buffer_skip_whitespace(input_buffer);
        if (!parse_value(current_item, input_buffer))
        {
            goto fail; /* 对象值解析失败。 */
        }
        buffer_skip_whitespace(input_buffer);
    }
    while (can_access_at_index(input_buffer, 0) && (buffer_at_offset(input_buffer)[0] == ','));

    if (cannot_access_at_index(input_buffer, 0) || (buffer_at_offset(input_buffer)[0] != '}'))
    {
        goto fail; /* 缺少对象结束符号。 */
    }

success:
    input_buffer->depth--;

    if (head != NULL) {
        head->prev = current_item;
    }

    item->type = cJSON_Object;
    item->child = head;

    input_buffer->offset++;
    return true;

fail:
    if (head != NULL)
    {
        cJSON_Delete(head);
    }

    return false;
}

/** @brief 递归打印成员键和值，按容器深度输出可选缩进。
 * @param[in] item 稳定借用对象节点。
 * @param[in,out] output_buffer 打印状态，成功恢复进入前 depth。
 * @return 完整输出真，深度/容量/成员失败假，外层统一撤销拥有块。
 */
static cJSON_bool print_object(const cJSON * const item, printbuffer * const output_buffer)
{
    unsigned char *output_pointer = NULL;
    size_t length = 0;
    cJSON *current_item = item->child;

    if (output_buffer == NULL)
    {
        return false;
    }

    if (output_buffer->depth >= CJSON_NESTING_LIMIT)
    {
        return false; /* 超过打印深度上限。 */
    }

    /* 格式化模式在左括号后追加换行。 */
    length = (size_t) (output_buffer->format ? 2 : 1);
    output_pointer = ensure(output_buffer, length + 1);
    if (output_pointer == NULL)
    {
        return false;
    }

    *output_pointer++ = '{';
    output_buffer->depth++;
    if (output_buffer->format)
    {
        *output_pointer++ = '\n';
    }
    output_buffer->offset += length;

    while (current_item)
    {
        if (output_buffer->format)
        {
            size_t i;
            output_pointer = ensure(output_buffer, output_buffer->depth);
            if (output_pointer == NULL)
            {
                return false;
            }
            for (i = 0; i < output_buffer->depth; i++)
            {
                *output_pointer++ = '\t';
            }
            output_buffer->offset += output_buffer->depth;
        }

        /* 打印键并合入片段长度。 */
        if (!print_string_ptr((unsigned char*)current_item->string, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        length = (size_t) (output_buffer->format ? 2 : 1);
        output_pointer = ensure(output_buffer, length);
        if (output_pointer == NULL)
        {
            return false;
        }
        *output_pointer++ = ':';
        if (output_buffer->format)
        {
            *output_pointer++ = '\t';
        }
        output_buffer->offset += length;

        /* 打印成员值并合入片段长度。 */
        if (!print_value(current_item, output_buffer))
        {
            return false;
        }
        update_offset(output_buffer);

        /* 后续仍有成员时写逗号，格式化模式另写换行。 */
        length = ((size_t)(output_buffer->format ? 1 : 0) + (size_t)(current_item->next ? 1 : 0));
        output_pointer = ensure(output_buffer, length + 1);
        if (output_pointer == NULL)
        {
            return false;
        }
        if (current_item->next)
        {
            *output_pointer++ = ',';
        }

        if (output_buffer->format)
        {
            *output_pointer++ = '\n';
        }
        *output_pointer = '\0';
        output_buffer->offset += length;

        current_item = current_item->next;
    }

    output_pointer = ensure(output_buffer, output_buffer->format ? (output_buffer->depth + 1) : 2);
    if (output_pointer == NULL)
    {
        return false;
    }
    if (output_buffer->format)
    {
        size_t i;
        for (i = 0; i < (output_buffer->depth - 1); i++)
        {
            *output_pointer++ = '\t';
        }
    }
    *output_pointer++ = '}';
    *output_pointer = '\0';
    output_buffer->depth--;

    return true;
}

/** @brief 遍历 child 链并转换为公开 int 计数。
 * @param[in] array 稳定父节点，NULL 返回零，不强制数组类型。
 * @return 成员数；超出 INT_MAX 时按 C 的 int 转换结果返回，调用者须限制规模。
 */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array)
{
    cJSON *child = NULL;
    size_t size = 0;

    if (array == NULL)
    {
        return 0;
    }

    child = array->child;

    while(child != NULL)
    {
        size++;
        child = child->next;
    }

    /* 公开计数类型为 int，超大集合无法由该接口完整表示。 */

    return (int)size;
}

/** @brief 以 size_t 下标在同级链中线性查找。
 * @param[in] array 稳定父节点，NULL 无结果。
 * @param[in] index 零基成员位置。
 * @return 借用成员或 NULL，不更改链与拥有权。
 */
static cJSON* get_array_item(const cJSON *array, size_t index)
{
    cJSON *current_child = NULL;

    if (array == NULL)
    {
        return NULL;
    }

    current_child = array->child;
    while ((current_child != NULL) && (index > 0))
    {
        index--;
        current_child = current_child->next;
    }

    return current_child;
}

/** @brief 拒绝负下标后委托成员查找。
 * @param[in] array 稳定借用父节点，NULL 无结果。
 * @param[in] index 零基下标，负值无结果。
 * @return 借用成员或 NULL，复杂度 O(n)。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index)
{
    if (index < 0)
    {
        return NULL;
    }

    return get_array_item(array, (size_t)index);
}

/** @brief 线性查找首个匹配键，保持大小写和首项兼容语义。
 * @param[in] object 稳定父节点，NULL 无结果。
 * @param[in] name 借用 C 键，NULL 无结果。
 * @param[in] case_sensitive 非零逐字节比较，零使用忽略大小写比较。
 * @return 借用成员或 NULL；区分大小写路径遇无键成员会停止。
 */
static cJSON *get_object_item(const cJSON * const object, const char * const name, const cJSON_bool case_sensitive)
{
    cJSON *current_element = NULL;

    if ((object == NULL) || (name == NULL))
    {
        return NULL;
    }

    current_element = object->child;
    if (case_sensitive)
    {
        while ((current_element != NULL) && (current_element->string != NULL) && (strcmp(name, current_element->string) != 0))
        {
            current_element = current_element->next;
        }
    }
    else
    {
        while ((current_element != NULL) && (case_insensitive_strcmp((const unsigned char*)name, (const unsigned char*)(current_element->string)) != 0))
        {
            current_element = current_element->next;
        }
    }

    if ((current_element == NULL) || (current_element->string == NULL)) {
        return NULL;
    }

    return current_element;
}

/** @brief 查找首个忽略大小写匹配的对象成员。
 * @param[in] object 稳定父节点，可为 NULL。
 * @param[in] string 借用键，NULL 无结果。
 * @return 借用成员或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, false);
}

/** @brief 查找首个区分大小写匹配的对象成员。
 * @param[in] object 稳定父节点，可为 NULL。
 * @param[in] string 借用键，NULL 无结果。
 * @return 借用成员或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string)
{
    return get_object_item(object, string, true);
}

/** @brief 将忽略大小写的成员查找结果转换为真假。
 * @param[in] object 稳定父节点，可为 NULL。
 * @param[in] string 借用键，可为 NULL。
 * @return 存在为真，否则为假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string)
{
    return cJSON_GetObjectItem(object, string) ? 1 : 0;
}

/** @brief 把 item 链接到已知尾节点后。
 * @param[in,out] prev 有效同级尾节点，不能为空。
 * @param[in,out] item 待挂接独占节点，不能为空。
 * @post prev.next 与 item.prev 互相连接，首节点尾缓存由调用者维护。
 */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/** @brief 创建只借用原内容的浅引用包装。
 * @param[in] item 稳定借用节点，NULL 失败。
 * @param[in] hooks 分配包装的稳定钩子。
 * @return 独占包装或 NULL；包装不借用原键且 next/prev 清空。
 */
static cJSON *create_reference(const cJSON *item, const internal_hooks * const hooks)
{
    cJSON *reference = NULL;
    if (item == NULL)
    {
        return NULL;
    }

    reference = cJSON_New_Item(hooks);
    if (reference == NULL)
    {
        return NULL;
    }

    memcpy(reference, item, sizeof(cJSON));
    reference->string = NULL;
    reference->type |= cJSON_IsReference;
    reference->next = reference->prev = NULL;
    return reference;
}

/** @brief 校验独立节点后按首节点尾缓存常数时间追加。
 * @param[in,out] array 可修改父节点，NULL 失败，child 链须有效且无环。
 * @param[in,out] item 独占未挂接节点，不得包含 array。
 * @return 成功真并转交节点；自环、已挂接节点或损坏尾缓存返回假且不更改链。
 */
static cJSON_bool add_item_to_array(cJSON *array, cJSON *item)
{
    cJSON *child = NULL;

    if ((item == NULL) || (array == NULL) || (array->type & cJSON_IsReference) || (array == item) ||
        (item->next != NULL) || (item->prev != NULL))
    {
        return false;
    }

    child = array->child;
    /* 首成员的 prev 保存尾节点，追加无需再次遍历。 */
    if (child == NULL)
    {
        /* 首个成员同时成为链首与链尾。 */
        array->child = item;
        item->prev = item;
        item->next = NULL;
    }
    else
    {
        if (child->prev == NULL)
        {
            return false;
        }
        /* 尾缓存已验证，连接新尾并更新首成员缓存。 */
        if (child->prev)
        {
            suffix_object(child->prev, item);
            array->child->prev = item;
        }
    }

    return true;
}

/** @brief 追加拥有节点，失败不消费 item。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in,out] item 独占未挂接节点，NULL 或自环失败。
 * @return 下层追加结果，成功归父节点，失败仍归调用者。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    return add_item_to_array(array, item);
}

#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic push
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif
/** @brief 为结构中的历史可写指针字段转换借用 const 地址。
 * @param[in] string 借用地址，可为 NULL，不在此写入或释放。
 * @return 相同地址，调用者须用拥有标志保留其只读/借用约束。
 */
static void* cast_away_const(const void* string)
{
    return (void*)string;
}
#if defined(__clang__) || (defined(__GNUC__)  && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ > 5))))
    #pragma GCC diagnostic pop
#endif


/** @brief 完成键准备后提交节点追加，失败前不破坏原键或拥有关系。
 * @param[in,out] object 可修改父节点，NULL 或损坏尾缓存失败。
 * @param[in] string 借用键，NULL 失败；常量模式须覆盖节点寿命。
 * @param[in,out] item 独占未挂接节点，不得包含 object。
 * @param[in] hooks 稳定钩子，用于复制和释放拥有键。
 * @param[in] constant_key 非零借用键，零深拷贝后接管。
 * @return 成功真，参数/OOM 失败假并保留原树与 item。
 */
static cJSON_bool add_item_to_object(cJSON * const object, const char * const string, cJSON * const item, const internal_hooks * const hooks, const cJSON_bool constant_key)
{
    char *new_key = NULL;
    int new_type = cJSON_Invalid;

    if ((object == NULL) || (object->type & cJSON_IsReference) || (string == NULL) || (item == NULL) || (object == item) ||
        (item->next != NULL) || (item->prev != NULL) ||
        ((object->child != NULL) && (object->child->prev == NULL)))
    {
        return false;
    }

    if (constant_key)
    {
        new_key = (char*)cast_away_const(string);
        new_type = item->type | cJSON_StringIsConst;
    }
    else
    {
        new_key = (char*)cJSON_strdup((const unsigned char*)string, hooks);
        if (new_key == NULL)
        {
            return false;
        }

        new_type = item->type & ~cJSON_StringIsConst;
    }

    if (!(item->type & cJSON_StringIsConst) && (item->string != NULL))
    {
        hooks->deallocate(item->string);
    }

    item->string = new_key;
    item->type = new_type;

    return add_item_to_array(object, item);
}

/** @brief 使用当前钩子复制键并转交独立成员节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 调用期间借用键，NULL 失败。
 * @param[in,out] item 独占未挂接节点，成功归父节点。
 * @return 成功真，失败假且不接管节点。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, false);
}

/** @brief 借用常量键并转交独立成员节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 长寿命只读键，不得借自本次释放的旧键。
 * @param[in,out] item 独占未挂接节点，成功归父节点。
 * @return 成功真，失败假，常量键始终由外部 owner 管理。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item)
{
    return add_item_to_object(object, string, item, &global_hooks, true);
}

/** @brief 新建浅引用后追加，失败归还包装并保留原内容。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] item 稳定借用内容节点，NULL 失败，内容覆盖引用使用期。
 * @return 成功真，失败假，原 item 始终不转交。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    cJSON *reference = NULL;
    if ((array == NULL) || (array->type & cJSON_IsReference))
    {
        return false;
    }

    reference = create_reference(item, &global_hooks);
    if (!add_item_to_array(array, reference))
    {
        cJSON_Delete(reference);
        return false;
    }
    return true;
}

/** @brief 新建浅引用并复制键挂接，键 OOM 时也归还包装。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 调用期间借用键，NULL 失败。
 * @param[in] item 稳定借用内容节点，内容须覆盖包装使用期。
 * @return 成功真，失败假，失败不消费原 item。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    cJSON *reference = NULL;
    if ((object == NULL) || (object->type & cJSON_IsReference) || (string == NULL))
    {
        return false;
    }

    reference = create_reference(item, &global_hooks);
    if (!add_item_to_object(object, string, reference, &global_hooks, false))
    {
        cJSON_Delete(reference);
        return false;
    }
    return true;
}

/** @brief 创建 null 成员，键或挂接失败时销毁新节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 已归父树的成员借用，失败 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name)
{
    cJSON *null = cJSON_CreateNull();
    if (add_item_to_object(object, name, null, &global_hooks, false))
    {
        return null;
    }

    cJSON_Delete(null);
    return NULL;
}

/** @brief 创建 True 成员并集中清理失败节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 父树内成员借用或 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name)
{
    cJSON *true_item = cJSON_CreateTrue();
    if (add_item_to_object(object, name, true_item, &global_hooks, false))
    {
        return true_item;
    }

    cJSON_Delete(true_item);
    return NULL;
}

/** @brief 创建 False 成员并清理失败节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 父树内成员借用或 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name)
{
    cJSON *false_item = cJSON_CreateFalse();
    if (add_item_to_object(object, name, false_item, &global_hooks, false))
    {
        return false_item;
    }

    cJSON_Delete(false_item);
    return NULL;
}

/** @brief 创建指定真假值成员，失败不发布部分节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] boolean 非零为 True，零为 False。
 * @return 父树内成员借用或 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean)
{
    cJSON *bool_item = cJSON_CreateBool(boolean);
    if (add_item_to_object(object, name, bool_item, &global_hooks, false))
    {
        return bool_item;
    }

    cJSON_Delete(bool_item);
    return NULL;
}

/** @brief 创建带饱和整数缓存的 Number 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] number 新数字，NaN 缓存零。
 * @return 父树内成员借用或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number)
{
    cJSON *number_item = cJSON_CreateNumber(number);
    if (add_item_to_object(object, name, number_item, &global_hooks, false))
    {
        return number_item;
    }

    cJSON_Delete(number_item);
    return NULL;
}

/** @brief 深拷贝文本并创建 String 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] string 借用 C 文本，NULL 失败。
 * @return 父树内成员借用或 NULL，任一分配失败清理全部新资源。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string)
{
    cJSON *string_item = cJSON_CreateString(string);
    if (add_item_to_object(object, name, string_item, &global_hooks, false))
    {
        return string_item;
    }

    cJSON_Delete(string_item);
    return NULL;
}

/** @brief 复制原始片段并创建 Raw 成员，不校验片段语法。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] raw 借用 NUL 片段，NULL 失败。
 * @return 父树内成员借用或 NULL，失败不发布新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw)
{
    cJSON *raw_item = cJSON_CreateRaw(raw);
    if (add_item_to_object(object, name, raw_item, &global_hooks, false))
    {
        return raw_item;
    }

    cJSON_Delete(raw_item);
    return NULL;
}

/** @brief 创建并挂接空对象成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 父树内成员借用或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name)
{
    cJSON *object_item = cJSON_CreateObject();
    if (add_item_to_object(object, name, object_item, &global_hooks, false))
    {
        return object_item;
    }

    cJSON_Delete(object_item);
    return NULL;
}

/** @brief 创建并挂接空数组成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 父树内成员借用或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name)
{
    cJSON *array = cJSON_CreateArray();
    if (add_item_to_object(object, name, array, &global_hooks, false))
    {
        return array;
    }

    cJSON_Delete(array);
    return NULL;
}

/** @brief 顺着父节点的成员链核对目标归属，不借助目标自身的邻接指针。
 * @param[in] parent 稳定父节点，NULL 返回假。
 * @param[in] item 待核对借用节点，NULL 返回假。
 * @return 目标确在 child 同级链中返回真，否则返回假，复杂度 O(n)。
 */
static cJSON_bool item_is_child(const cJSON *parent, const cJSON *item)
{
    const cJSON *child = parent ? parent->child : NULL;
    while (child != NULL)
    {
        if (child == item)
        {
            return true;
        }
        child = child->next;
    }
    return false;
}

/** @brief 先线性验证父链归属，再维护邻接和首成员尾缓存。
 * @param[in,out] parent 可修改父节点，NULL 失败。
 * @param[in,out] item 待分离成员，NULL 或属于其它父链时失败。
 * @return 独占分离节点，next/prev 清空；失败 NULL，原树不变。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item)
{
    if ((parent == NULL) || (parent->type & cJSON_IsReference) || (item == NULL) || !item_is_child(parent, item) ||
        ((item != parent->child) && (item->prev == NULL)))
    {
        return NULL;
    }

    if (item != parent->child)
    {
        /* 非首成员需更新前驱的 next。 */
        item->prev->next = item->next;
    }
    if (item->next != NULL)
    {
        /* 非尾成员需更新后继的 prev。 */
        item->next->prev = item->prev;
    }

    if (item == parent->child)
    {
        /* 首成员移走后更新父节点 child。 */
        parent->child = item->next;
    }
    else if (item->next == NULL)
    {
        /* 尾成员移走后更新首成员尾缓存。 */
        parent->child->prev = item->prev;
    }

    /* 清除同级指针，使其可以合法重新挂接。 */
    item->prev = NULL;
    item->next = NULL;

    return item;
}

/** @brief 查找非负下标并委托带归属核对的分离。
 * @param[in,out] array 可修改父节点，NULL 无结果。
 * @param[in] which 零基下标，负值或越界无结果。
 * @return 独占节点或 NULL，不释放分离出的资源。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which)
{
    if (which < 0)
    {
        return NULL;
    }

    return cJSON_DetachItemViaPointer(array, get_array_item(array, (size_t)which));
}

/** @brief 分离下标成员后归还其拥有资源。
 * @param[in,out] array 可修改父节点，NULL 不操作。
 * @param[in] which 零基下标，负值/越界不操作。
 * @post 拥有型父的匹配节点已归还；引用父不操作，其它成员保留。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

/** @brief 忽略大小写找到首成员后转回调用者拥有。
 * @param[in,out] object 可修改父节点，NULL 无结果。
 * @param[in] string 借用键，NULL 无结果。
 * @return 独占分离节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItem(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

/** @brief 区分大小写找到首成员后分离。
 * @param[in,out] object 可修改父节点，NULL 无结果。
 * @param[in] string 借用键，NULL 无结果。
 * @return 独占节点或 NULL，成功清空同级指针。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON *to_detach = cJSON_GetObjectItemCaseSensitive(object, string);

    return cJSON_DetachItemViaPointer(object, to_detach);
}

/** @brief 忽略大小写分离并释放首个匹配成员。
 * @param[in,out] object 可修改父节点，NULL 不操作。
 * @param[in] string 借用键，NULL 不操作。
 * @post 拥有型父的匹配成员已释放，找不到或引用父时原树保持。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

/** @brief 区分大小写分离并释放首个匹配成员。
 * @param[in,out] object 可修改父节点，NULL 不操作。
 * @param[in] string 借用键，NULL 不操作。
 * @post 拥有型父的匹配成员归还，引用父和其余内容保留。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObjectCaseSensitive(object, string));
}

/** @brief 查找插入位置并维护双向链，下标在末尾后则追加。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] which 非负零基下标。
 * @param[in,out] newitem 独占未挂接节点，不得包含父节点。
 * @return 成功真并转交拥有权，失败假且保留原树与新节点。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *after_inserted = NULL;

    if ((array != NULL && (array->type & cJSON_IsReference)) || which < 0 || newitem == NULL || newitem == array ||
        newitem->next != NULL || newitem->prev != NULL)
    {
        return false;
    }

    after_inserted = get_array_item(array, (size_t)which);
    if (after_inserted == NULL)
    {
        return add_item_to_array(array, newitem);
    }

    if (after_inserted != array->child && after_inserted->prev == NULL) {
        /* 已有成员链的前驱关系损坏时不提交。 */
        return false;
    }

    newitem->next = after_inserted;
    newitem->prev = after_inserted->prev;
    after_inserted->prev = newitem;
    if (after_inserted == array->child)
    {
        array->child = newitem;
    }
    else
    {
        newitem->prev->next = newitem;
    }
    return true;
}

/** @brief 线性核对旧成员归属后以独立节点替换并释放旧节点。
 * @param[in,out] parent 可修改父节点，NULL 失败。
 * @param[in,out] item parent 的成员，外来节点失败并保留两树。
 * @param[in,out] replacement 独占未挂接节点，等于 item 时保留原节点并成功。
 * @return 成功真；参数、自环、重复挂接失败假且不消费 replacement。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement)
{
    if ((parent == NULL) || (parent->type & cJSON_IsReference) ||
        (replacement == NULL) || (item == NULL) || !item_is_child(parent, item))
    {
        return false;
    }

    if (replacement == item)
    {
        return true;
    }

    if ((replacement == parent) || (replacement->next != NULL) || (replacement->prev != NULL))
    {
        return false;
    }

    replacement->next = item->next;
    replacement->prev = item->prev;

    if (replacement->next != NULL)
    {
        replacement->next->prev = replacement;
    }
    if (parent->child == item)
    {
        if (parent->child->prev == parent->child)
        {
            replacement->prev = replacement;
        }
        parent->child = replacement;
    }
    else
    {   /* 非首成员正常更新前驱，尾替换同时维护首成员的尾缓存。 */
        if (replacement->prev != NULL)
        {
            replacement->prev->next = replacement;
        }
        if (replacement->next == NULL)
        {
            parent->child->prev = replacement;
        }
    }

    item->next = NULL;
    item->prev = NULL;
    cJSON_Delete(item);

    return true;
}

/** @brief 按非负下标替换数组成员。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] which 零基下标，负值或越界失败。
 * @param[in,out] newitem 独占未挂接新节点，可等于原成员。
 * @return 成功真，失败假并保留新节点拥有权。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    if (which < 0)
    {
        return false;
    }

    return cJSON_ReplaceItemViaPointer(array, get_array_item(array, (size_t)which), newitem);
}

/** @brief 先查找目标并复制可能借自旧键的查找文本，再提交替换。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 借用键，NULL 失败，可借自任一待替换键。
 * @param[in,out] replacement 独占未挂接节点，可等于目标自身。
 * @param[in] case_sensitive 是否区分键大小写。
 * @return 成功真，参数/缺键/OOM 失败假且保留两端原有状态。
 */
static cJSON_bool replace_item_in_object(cJSON *object, const char *string, cJSON *replacement, cJSON_bool case_sensitive)
{
    cJSON *existing = NULL;
    char *new_key = NULL;
    if ((object == NULL) || (object->type & cJSON_IsReference) || (replacement == NULL) || (string == NULL))
    {
        return false;
    }

    existing = get_object_item(object, string, case_sensitive);
    if ((existing == NULL) || (replacement == object) ||
        ((replacement != existing) && ((replacement->next != NULL) || (replacement->prev != NULL))))
    {
        return false;
    }
    new_key = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
    if (new_key == NULL)
    {
        return false;
    }
    /* 键可借自 replacement，先完成查找和复制再归还原键。 */
    if (!(replacement->type & cJSON_StringIsConst) && (replacement->string != NULL))
    {
        cJSON_free(replacement->string);
    }
    replacement->string = new_key;

    replacement->type &= ~cJSON_StringIsConst;

    return cJSON_ReplaceItemViaPointer(object, existing, replacement);
}

/** @brief 忽略大小写替换首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 借用键，NULL 失败。
 * @param[in,out] newitem 独占替代节点，可为匹配节点本身。
 * @return 成功真，失败假且保留调用者拥有权。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, false);
}

/** @brief 区分大小写替换首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 借用键，NULL 失败。
 * @param[in,out] newitem 独占替代节点，可为匹配节点本身。
 * @return 成功真，失败假并保留原树与新节点。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object, const char *string, cJSON *newitem)
{
    return replace_item_in_object(object, string, newitem, true);
}

/** @brief 申请并标记独占 null 节点。
 * @return 独占节点或 NULL，失败无部分结果。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_NULL;
    }

    return item;
}

/** @brief 申请并标记 True，数字缓存保持零初始化。
 * @return 独占节点或 NULL，布尔值应按类型读取。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_True;
    }

    return item;
}

/** @brief 申请独占 False 节点。
 * @return 独占节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_False;
    }

    return item;
}

/** @brief 按真假值选择新节点布尔类型。
 * @param[in] boolean 非零为 True，零为 False。
 * @return 独占节点或 NULL，不将布尔值写入整数缓存。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = boolean ? cJSON_True : cJSON_False;
    }

    return item;
}

/** @brief 创建 double 节点并建立可表示的 int 缓存。
 * @param[in] num 数值，NaN 缓存零，无穷和超界按 int 两端饱和。
 * @return 独占节点或 NULL，原 double 包括 NaN 保留供打印分类。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Number;
        item->valuedouble = num;

        /* 先处理 NaN，避免执行未定义的 NaN 到 int 转换。 */
        if (isnan(num))
        {
            item->valueint = 0;
        }
        else if (num >= INT_MAX)
        {
            item->valueint = INT_MAX;
        }
        else if (num <= (double)INT_MIN)
        {
            item->valueint = INT_MIN;
        }
        else
        {
            item->valueint = (int)num;
        }
    }

    return item;
}

/** @brief 分别申请节点与完整 C 文本副本。
 * @param[in] string 稳定借用 NUL 文本，NULL 失败。
 * @return 独占 String 或 NULL，文本复制失败时一并归还节点。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_String;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)string, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

/** @brief 创建只借用文本的 String 包装。
 * @param[in] string 长寿命借用文本，可为 NULL，不在包装删除时释放。
 * @return 独占引用包装或 NULL，外部 owner 保持文本稳定。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL)
    {
        item->type = cJSON_String | cJSON_IsReference;
        item->valuestring = (char*)cast_away_const(string);
    }

    return item;
}

/** @brief 创建借用成员链的对象包装。
 * @param[in] child 稳定成员链首，可为 NULL，须覆盖包装使用期。
 * @return 独占包装或 NULL，删除包装保留被引用内容。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Object | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

/** @brief 创建借用元素链的数组包装。
 * @param[in] child 稳定元素链首，可为 NULL，外部 owner 继续管理。
 * @return 独占包装或 NULL，Delete 不递归释放 child。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child) {
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item != NULL) {
        item->type = cJSON_Array | cJSON_IsReference;
        item->child = (cJSON*)cast_away_const(child);
    }

    return item;
}

/** @brief 复制原始 JSON 片段，不作解析校验。
 * @param[in] raw 借用 NUL 文本，NULL 失败。
 * @return 独占 Raw 或 NULL，文本复制失败时归还节点。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type = cJSON_Raw;
        item->valuestring = (char*)cJSON_strdup((const unsigned char*)raw, &global_hooks);
        if(!item->valuestring)
        {
            cJSON_Delete(item);
            return NULL;
        }
    }

    return item;
}

/** @brief 申请空数组节点。
 * @return 独占节点或 NULL，child 初始为 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if(item)
    {
        item->type=cJSON_Array;
    }

    return item;
}

/** @brief 申请空对象节点。
 * @return 独占节点或 NULL，child 初始为 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item(&global_hooks);
    if (item)
    {
        item->type = cJSON_Object;
    }

    return item;
}

/** @brief 逐项创建 int 数组，最后建立首成员尾缓存。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败。
 * @param[in] count 元素数，负值失败，零生成空数组。
 * @return 独占完整数组或 NULL，分配失败删除全部已建成员。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if (!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/** @brief 将 float 逐项提升为 double 并创建数组。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败。
 * @param[in] count 元素数，负值失败。
 * @return 独占数组或 NULL，失败清理部分节点。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber((double)numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/** @brief 逐项创建 double 数组，保留各数字的分类与饱和缓存。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败。
 * @param[in] count 元素数，负值失败。
 * @return 独占数组或 NULL，失败归还部分数组。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (numbers == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for(i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateNumber(numbers[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p, n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/** @brief 深拷贝每个 C 文本并创建字符串数组。
 * @param[in] strings 至少含 count 个借用文本，数组或任一文本为 NULL 失败。
 * @param[in] count 元素数，负值失败。
 * @return 独占完整数组或 NULL，失败归还所有已复制文本和节点。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count)
{
    size_t i = 0;
    cJSON *n = NULL;
    cJSON *p = NULL;
    cJSON *a = NULL;

    if ((count < 0) || (strings == NULL))
    {
        return NULL;
    }

    a = cJSON_CreateArray();

    for (i = 0; a && (i < (size_t)count); i++)
    {
        n = cJSON_CreateString(strings[i]);
        if(!n)
        {
            cJSON_Delete(a);
            return NULL;
        }
        if(!i)
        {
            a->child = n;
        }
        else
        {
            suffix_object(p,n);
        }
        p = n;
    }

    if (a && a->child) {
        a->child->prev = n;
    }

    return a;
}

/** @brief 递归复制单节点及可选 child，供公开副本入口调用。
 * @param[in] item 稳定借用节点，NULL 失败。
 * @param[in] depth 当前 child 边数，继续递归前检查 CIRCULAR_LIMIT。
 * @param[in] recurse 非零复制 child，零仅复制本节点与字符串。
 * @return 独占副本或 NULL，常量键仍借用，失败清理部分副本。
 */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse);

/** @brief 从零深度开始复制单节点及可选子树。
 * @param[in] item 稳定借用节点，NULL 失败，不复制其同级 next 链。
 * @param[in] recurse 非零递归复制 child，零只复制当前节点和字符串。
 * @return 独占副本或 NULL，常量成员键仍借用源键。
 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse)
{
    return cJSON_Duplicate_rec(item, 0, recurse );
}

/** @brief 按 child 深度复制，失败递归清理未完成副本。
 * @param[in] item 稳定源节点，NULL 失败。
 * @param[in] depth 已经过的 child 边数，继续递归前检查 CIRCULAR_LIMIT。
 * @param[in] recurse 是否复制 child 链。
 * @return 独占副本或 NULL，引用值/子树转为拥有，常量键仍借用。
 * @note 不检测 next 环；深度上限独立于解析/打印上限，栈预算由调用者保证。
 */
cJSON * cJSON_Duplicate_rec(const cJSON *item, size_t depth, cJSON_bool recurse)
{
    cJSON *newitem = NULL;
    cJSON *child = NULL;
    cJSON *next = NULL;
    cJSON *newchild = NULL;

    /* 空源没有副本。 */
    if (!item)
    {
        goto fail;
    }
    /* 申请尚未发布的独占副本。 */
    newitem = cJSON_New_Item(&global_hooks);
    if (!newitem)
    {
        goto fail;
    }
    /* 复制值并去除内容引用标志，副本文本改为独占。 */
    newitem->type = item->type & (~cJSON_IsReference);
    newitem->valueint = item->valueint;
    newitem->valuedouble = item->valuedouble;
    if (item->valuestring)
    {
        newitem->valuestring = (char*)cJSON_strdup((unsigned char*)item->valuestring, &global_hooks);
        if (!newitem->valuestring)
        {
            goto fail;
        }
    }
    if (item->string)
    {
        newitem->string = (item->type&cJSON_StringIsConst) ? item->string : (char*)cJSON_strdup((unsigned char*)item->string, &global_hooks);
        if (!newitem->string)
        {
            goto fail;
        }
    }
    /* 非递归模式不复制 child 链。 */
    if (!recurse)
    {
        return newitem;
    }
    /* 当前层遍历 child 的同级链，每次只递归复制一个成员。 */
    child = item->child;
    while (child != NULL)
    {
        if(depth >= CJSON_CIRCULAR_LIMIT) {
            goto fail;
        }
        newchild = cJSON_Duplicate_rec(child, depth + 1, true); /* 复制当前成员及其子树。 */
        if (!newchild)
        {
            goto fail;
        }
        if (next != NULL)
        {
            /* 接入已有副本成员链并推进尾指针。 */
            next->next = newchild;
            newchild->prev = next;
            next = newchild;
        }
        else
        {
            /* 首个副本成员交给新父节点。 */
            newitem->child = newchild;
            next = newchild;
        }
        child = child->next;
    }
    if (newitem && newitem->child)
    {
        newitem->child->prev = newchild;
    }

    return newitem;

fail:
    if (newitem != NULL)
    {
        cJSON_Delete(newitem);
    }

    return NULL;
}

/** @brief 越过双斜杠注释，包含存在的末尾换行。
 * @param[in,out] input 指向有效注释起始位置的游标槽。
 * @post 游标停在下一行首或尾 NUL，不修改文本。
 */
static void skip_oneline_comment(char **input)
{
    *input += static_strlen("//");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if ((*input)[0] == '\n') {
            *input += static_strlen("\n");
            return;
        }
    }
}

/** @brief 越过块注释直到闭合符或尾 NUL。
 * @param[in,out] input 指向有效块注释首位置的游标槽。
 * @post 游标停在闭合符后，缺少闭合符则停在 NUL，不报告语法错误。
 */
static void skip_multiline_comment(char **input)
{
    *input += static_strlen("/*");

    for (; (*input)[0] != '\0'; ++(*input))
    {
        if (((*input)[0] == '*') && ((*input)[1] == '/'))
        {
            *input += static_strlen("*/");
            return;
        }
    }
}

/** @brief 原样前移一个带引号片段，转义字节对共同推进。
 * @param[in,out] input 指向首引号的源游标，遇尾 NUL 也停止。
 * @param[in,out] output 同一可写缓冲中不晚于 input 的目标游标。
 * @post 游标越过结束引号或到达尾 NUL，保留转义原文，不执行解码校验。
 */
static void minify_string(char **input, char **output) {
    (*output)[0] = (*input)[0];
    *input += static_strlen("\"");
    *output += static_strlen("\"");


    for (; (*input)[0] != '\0'; (void)++(*input), ++(*output)) {
        (*output)[0] = (*input)[0];

        if ((*input)[0] == '\"') {
            (*output)[0] = '\"';
            *input += static_strlen("\"");
            *output += static_strlen("\"");
            return;
        } else if (((*input)[0] == '\\') && ((*input)[1] != '\0')) {
            (*output)[1] = (*input)[1];
            *input += static_strlen("\"");
            *output += static_strlen("\"");
        }
    }
}

/** @brief 就地移除字符串外空白和注释，保留原文转义。
 * @param[in,out] json 独占可写 C 文本，NULL 不操作。
 * @post 目标前移并 NUL 终止，不分配、不返回错误，不保证校验损坏 JSON。
 */
CJSON_PUBLIC(void) cJSON_Minify(char *json)
{
    char *into = json;

    if (json == NULL)
    {
        return;
    }

    while (json[0] != '\0')
    {
        switch (json[0])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                json++;
                break;

            case '/':
                if (json[1] == '/')
                {
                    skip_oneline_comment(&json);
                }
                else if (json[1] == '*')
                {
                    skip_multiline_comment(&json);
                } else {
                    json++;
                }
                break;

            case '\"':
                minify_string(&json, (char**)&into);
                break;

            default:
                into[0] = json[0];
                json++;
                into++;
        }
    }

    /* 在新的压缩末尾补 NUL。 */
    *into = '\0';
}

/** @brief 比较基础类型是否为 Invalid。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 基础类型匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Invalid;
}

/** @brief 比较基础类型是否为 False。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_False;
}

/** @brief 比较基础类型是否为 True。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xff) == cJSON_True;
}


/** @brief 检查 True/False 类型位是否存在。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 含任一布尔位真，否则假，保留原位判断语义。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & (cJSON_True | cJSON_False)) != 0;
}
/** @brief 比较基础类型是否为 JSON null。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_NULL;
}

/** @brief 比较基础类型是否为 Number。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Number;
}

/** @brief 比较基础类型是否为 String。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_String;
}

/** @brief 比较基础类型是否为 Array。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Array;
}

/** @brief 比较基础类型是否为 Object。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Object;
}

/** @brief 比较基础类型是否为 Raw。
 * @param[in] item 稳定借用节点，NULL 为假。
 * @return 匹配真，否则假。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item)
{
    if (item == NULL)
    {
        return false;
    }

    return (item->type & 0xFF) == cJSON_Raw;
}

/** @brief 按值递归比较树，对象通过双向首键查找忽略成员顺序。
 * @param[in] a 稳定借用树，NULL 或无效类型返回假。
 * @param[in] b 稳定借用树，NULL 或类型不同返回假。
 * @param[in] case_sensitive 非零区分对象键大小写，零忽略。
 * @return 相等真，否则假；数组顺序敏感，数值按相对误差比较。
 * @pre 两树无 child/next 环且深度符合调用者栈预算，不单独施加解析深度门。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive)
{
    if ((a == NULL) || (b == NULL) || ((a->type & 0xFF) != (b->type & 0xFF)))
    {
        return false;
    }

    /* 只接受已知基础类型。 */
    switch (a->type & 0xFF)
    {
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
        case cJSON_Number:
        case cJSON_String:
        case cJSON_Raw:
        case cJSON_Array:
        case cJSON_Object:
            break;

        default:
            return false;
    }

    /* 同一个有效节点无需再次遍历。 */
    if (a == b)
    {
        return true;
    }

    switch (a->type & 0xFF)
    {
        /* 布尔和 null 只需基础类型相同。 */
        case cJSON_False:
        case cJSON_True:
        case cJSON_NULL:
            return true;

        case cJSON_Number:
            if (compare_double(a->valuedouble, b->valuedouble))
            {
                return true;
            }
            return false;

        case cJSON_String:
        case cJSON_Raw:
            if ((a->valuestring == NULL) || (b->valuestring == NULL))
            {
                return false;
            }
            if (strcmp(a->valuestring, b->valuestring) == 0)
            {
                return true;
            }

            return false;

        case cJSON_Array:
        {
            cJSON *a_element = a->child;
            cJSON *b_element = b->child;

            for (; (a_element != NULL) && (b_element != NULL);)
            {
                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }

                a_element = a_element->next;
                b_element = b_element->next;
            }

            /* 双方必须同时结束，避免把数组前缀当成相等。 */
            if (a_element != b_element) {
                return false;
            }

            return true;
        }

        case cJSON_Object:
        {
            cJSON *a_element = NULL;
            cJSON *b_element = NULL;
            cJSON_ArrayForEach(a_element, a)
            {
                /* 对象逐键线性查找，最坏 O(n²)，重复键保留首匹配行为。 */
                b_element = get_object_item(b, a_element->string, case_sensitive);
                if (b_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(a_element, b_element, case_sensitive))
                {
                    return false;
                }
            }

            /* 反向再查一次，避免仅一方是另一方子集时返回相等。 */
            cJSON_ArrayForEach(b_element, b)
            {
                a_element = get_object_item(a, b_element->string, case_sensitive);
                if (a_element == NULL)
                {
                    return false;
                }

                if (!cJSON_Compare(b_element, a_element, case_sensitive))
                {
                    return false;
                }
            }

            return true;
        }

        default:
            return false;
    }
}

/** @brief 调用当前共享分配钩子。
 * @param[in] size 请求字节数，零沿钩子行为。
 * @return 独占块或 NULL，后续由相容释放器归还。
 */
CJSON_PUBLIC(void *) cJSON_malloc(size_t size)
{
    return global_hooks.allocate(size);
}

/** @brief 调用当前共享释放钩子，不改写调用者指针槽。
 * @param[in] object 配对独占块或 NULL。
 * @post 对应分配已归还，本地置空不影响调用者变量。
 */
CJSON_PUBLIC(void) cJSON_free(void *object)
{
    global_hooks.deallocate(object);
    object = NULL;
}

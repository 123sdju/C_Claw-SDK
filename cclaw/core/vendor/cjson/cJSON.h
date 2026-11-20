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

/** @file cJSON 1.7.19 的节点和树操作接口，版权与许可原文保留于文件开头。
 * @note 返回的独占树用 cJSON_Delete 释放，独占打印文本用匹配分配钩子的
 * cJSON_free 释放。查询返回借用，父树修改或销毁后重新取得。
 * 独立树可并行使用，同一树的修改、遍历与销毁由调用者同步。
 * InitHooks 只在关闭所有入口且无存活节点和打印输出时配置。
 * 数字转换及忽略大小写比较依赖进程 locale，调用期间不得并发 setlocale。
 * 未启用 ENABLE_LOCALES 时，数值 locale 须采用点号小数分隔符。
 * IsReference 父节点的 child 链只读，Add/Insert/Replace/Detach 返回失败，
 * DeleteItem 包装不操作；引用包装本身仍可在拥有型父节点中分离或替换。
 */
#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#ifdef __WINDOWS__

/* Windows 固定调用约定。HIDE 不导出，EXPORT 在库侧导出，IMPORT 在使用侧导入。
 * Unix 可配合编译器默认隐藏选项与 CJSON_API_VISIBILITY 显式导出公共符号。 */

#define CJSON_CDECL __cdecl
#define CJSON_STDCALL __stdcall

/* 未指定可见性时默认导出，支持直接编译这一对源头文件。 */
#if !defined(CJSON_HIDE_SYMBOLS) && !defined(CJSON_IMPORT_SYMBOLS) && !defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_EXPORT_SYMBOLS
#endif

#if defined(CJSON_HIDE_SYMBOLS)
#define CJSON_PUBLIC(type)   type CJSON_STDCALL
#elif defined(CJSON_EXPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllexport) type CJSON_STDCALL
#elif defined(CJSON_IMPORT_SYMBOLS)
#define CJSON_PUBLIC(type)   __declspec(dllimport) type CJSON_STDCALL
#endif
#else /* 非 Windows 平台。 */
#define CJSON_CDECL
#define CJSON_STDCALL

#if (defined(__GNUC__) || defined(__SUNPRO_CC) || defined (__SUNPRO_C)) && defined(CJSON_API_VISIBILITY)
#define CJSON_PUBLIC(type)   __attribute__((visibility("default"))) type
#else
#define CJSON_PUBLIC(type) type
#endif
#endif

/** 该源码与头文件共同维护的版本号，构建时检查一致性。 */
#define CJSON_VERSION_MAJOR 1
#define CJSON_VERSION_MINOR 7
#define CJSON_VERSION_PATCH 19

#include <stddef.h>

/** 节点低八位的基础类型，拥有权标志另占高位。 */
#define cJSON_Invalid (0)
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw    (1 << 7) /* 打印时直接复制的 JSON 片段，创建时不校验语法。 */

/** 借用 child 与 valuestring，删除此节点时保留其指向资源。 */
#define cJSON_IsReference 256
/** 借用成员键 string，删除此节点时保留该键。 */
#define cJSON_StringIsConst 512

/** 节点可独占子树，也可由标志注明借用。拥有链须无环且只归一个父节点。 */
typedef struct cJSON
{
    /* 同级双向链，首节点 prev 缓存尾节点，独立节点的两指针为 NULL。 */
    struct cJSON *next;
    struct cJSON *prev;
    /* 数组元素或对象成员链首，引用节点只借用这一链。 */
    struct cJSON *child;

    /* 基础类型与 cJSON_IsReference/cJSON_StringIsConst 标志的组合。 */
    int type;

    /* String/Raw 的 NUL 终止内容，拥有或借用由 IsReference 决定。 */
    char *valuestring;
    /* 整数缓存，优先用 SetNumberValue 同步更新双精度值与饱和缓存。 */
    int valueint;
    /* Number 的双精度值，可保存 NaN/无穷，打印时两者转为 null。 */
    double valuedouble;

    /* 对象成员键，StringIsConst 标志决定是否随节点释放。 */
    char *string;
} cJSON;

typedef struct cJSON_Hooks
{
      /** @brief 按字节申请独占内存，Windows 使用 CDECL。
       * @param[in] sz 请求字节数，可能为零。
       * @return 成功返回满足标准分配对齐的内存，失败返回 NULL。
       * @note 与 free_fn 成对，可能由不同线程调用，配置前保证其线程安全。
       */
      void *(CJSON_CDECL *malloc_fn)(size_t sz);
      /** @brief 释放由配对 malloc_fn 取得的独占内存。
       * @param[in] ptr 待释放地址，须接受 NULL。
       * @post 对应分配已归还，回调不保存该地址。
       */
      void (CJSON_CDECL *free_fn)(void *ptr);
} cJSON_Hooks;

/** 真假返回值，零为假，非零为真。 */
typedef int cJSON_bool;

/** 解析和打印允许的对象/数组容器层数，根容器计一层，超限失败。
 * SDK 默认 1000，固件通过构建选项覆盖为 32；完整栈预算另含调用者帧。 */
#ifndef CJSON_NESTING_LIMIT
#define CJSON_NESTING_LIMIT 1000
#endif

/** 递归复制的 child 深度上限，阻止无限 child 引用递归。
 * 不检查同级 next 环，Delete/Compare 也不使用这个限制。 */
#ifndef CJSON_CIRCULAR_LIMIT
#define CJSON_CIRCULAR_LIMIT 10000
#endif

/** @brief 取得编译版本的只读常量文本。
 * @return 进程生命周期有效的借用字符串，可由多个线程并行读取。
 */
CJSON_PUBLIC(const char*) cJSON_Version(void);

/** @brief 安装进程共享分配钩子，缺失的函数采用标准 malloc/free。
 * @param[in] hooks 可选钩子表，仅在调用期间借用，NULL 恢复标准分配器。
 * @pre 所有 cJSON 入口已排空，先释放依赖当前钩子的节点和输出。
 * @post 全局分配器已更新；仅标准 malloc/free 配对时启用标准 realloc。
 */
CJSON_PUBLIC(void) cJSON_InitHooks(cJSON_Hooks* hooks);

/** @brief 从 NUL 终止文本解析第一个值，保留尾部兼容行为。
 * @param[in] value 调用期间借用的文本，NULL 失败。
 * @return 成功返回独占树，失败返回 NULL 并清理部分树。
 * @note 允许合法解码 NUL，字符串接口只暴露其首 NUL 前缀；SDK 严格入口另行拒绝。
 */
CJSON_PUBLIC(cJSON *) cJSON_Parse(const char *value);
/** @brief 在显式可读范围内解析第一个值，无需尾部 NUL。
 * @param[in] value 可读文本，NULL 失败，仅在调用期间借用。
 * @param[in] buffer_length 可读字节数，零失败，必须覆盖实际访问范围。
 * @return 成功返回独占树，失败返回 NULL，允许未消费尾部。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLength(const char *value, size_t buffer_length);
/** @brief 解析 C 文本并按选项检查值后是否只剩空白和 NUL。
 * @param[in] value NUL 终止输入，NULL 失败。
 * @param[out] return_parse_end 可选借用输出，成功指向值后，非空输入失败指向错误附近。
 * @param[in] require_null_terminated 非零时要求跳过空白后遇到 NUL。
 * @return 独占树或 NULL，错误位置依赖输入文本继续存活。
 * @note NULL 输入保留结束槽与旧线程错误。该选项不收紧数字和空白语法，
 * SDK 严格检查由其包装层提供。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithOpts(const char *value, const char **return_parse_end, cJSON_bool require_null_terminated);
/** @brief 在字节范围内解析并输出调用专属的结束或错误位置。
 * @param[in] value 可读输入，NULL 失败。
 * @param[in] buffer_length 实际可读字节数，零失败；要求 NUL 时应包含其字节。
 * @param[out] return_parse_end 可选借用位置，成功指向值后或跳过的空白后，NULL 输入保留原槽。
 * @param[in] require_null_terminated 非零要求范围内值后空白结束于 NUL。
 * @return 成功返回独占树，失败返回 NULL 并释放全部部分节点。
 * @note 本线程最近解析错误同时供 GetErrorPtr 查询，输入须保持有效。
 */
CJSON_PUBLIC(cJSON *) cJSON_ParseWithLengthOpts(const char *value, size_t buffer_length, const char **return_parse_end, cJSON_bool require_null_terminated);

/** @brief 格式化打印一棵稳定树。
 * @param[in] item 借用树，NULL、无效类型、超深度或分配失败均失败。
 * @return 独占 NUL 终止文本，失败 NULL；用匹配钩子的 cJSON_free 归还。
 */
CJSON_PUBLIC(char *) cJSON_Print(const cJSON *item);
/** @brief 紧凑打印稳定树。
 * @param[in] item 调用期间借用的树，NULL 失败。
 * @return 独占文本或 NULL，失败不交付部分输出。
 */
CJSON_PUBLIC(char *) cJSON_PrintUnformatted(const cJSON *item);
/** @brief 从预计容量起步动态打印，输出容量按需增长。
 * @param[in] item 稳定借用树，NULL 失败。
 * @param[in] prebuffer 初始容量字节数，负值失败，零依赖分配器的零字节行为。
 * @param[in] fmt 非零格式化，零紧凑打印。
 * @return 独占 NUL 文本或 NULL，失败归还已分配缓冲。
 */
CJSON_PUBLIC(char *) cJSON_PrintBuffered(const cJSON *item, int prebuffer, cJSON_bool fmt);
/** @brief 在调用者缓冲内打印，始终保留缓冲拥有权。
 * @param[in] item 稳定借用树，NULL 失败。
 * @param[out] buffer 可写缓冲，NULL 失败，失败时可含未完成文本。
 * @param[in] length 缓冲总字节数，含 NUL；负值失败，建议比预计文本多留五字节。
 * @param[in] format 非零格式化，零紧凑打印。
 * @return 完整打印返回 1，参数、容量、深度或类型失败返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_PrintPreallocated(cJSON *item, char *buffer, const int length, const cJSON_bool format);
/** @brief 释放节点及其后续同级链，递归归还拥有子树和字符串。
 * @param[in] item 独占节点或 NULL；父节点中的成员应先 detach。
 * @post 所有拥有资源已归还，引用 child/valuestring 与常量键保留。
 * @pre 链和拥有关系须无环，释放期间不得有其它读写者。
 */
CJSON_PUBLIC(void) cJSON_Delete(cJSON *item);

/** @brief 计数 child 同级链，数组与对象均可使用。
 * @param[in] array 稳定节点或 NULL，NULL 返回零。
 * @return 成员数；超过 INT_MAX 时受 int 返回类型限制，调用者须限制规模。
 */
CJSON_PUBLIC(int) cJSON_GetArraySize(const cJSON *array);
/** @brief 按零基下标取得 child 链中的成员，复杂度 O(n)。
 * @param[in] array 稳定父节点或 NULL，不强制检查基础类型。
 * @param[in] index 零基下标，负值无结果。
 * @return 借用成员，越界或参数无效返回 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetArrayItem(const cJSON *array, int index);
/** @brief 忽略大小写查找首个同名成员，复杂度 O(n)。
 * @param[in] object 稳定父节点或 NULL。
 * @param[in] string NUL 终止的借用键，NULL 无结果。
 * @return 借用成员或 NULL，重复键保留首项语义。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItem(const cJSON * const object, const char * const string);
/** @brief 逐字节区分大小写查找首个成员。
 * @param[in] object 稳定父节点或 NULL。
 * @param[in] string 调用期间借用的键，NULL 无结果。
 * @return 借用成员或 NULL，复杂度 O(n)。
 */
CJSON_PUBLIC(cJSON *) cJSON_GetObjectItemCaseSensitive(const cJSON * const object, const char * const string);
/** @brief 检查是否存在忽略大小写匹配的成员。
 * @param[in] object 稳定父节点或 NULL。
 * @param[in] string 借用键，NULL 返回零。
 * @return 找到返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_HasObjectItem(const cJSON *object, const char *string);
/** @brief 查询本线程最近一次解析记录的错误位置。
 * @return 输入缓冲中的借用地址，无错误返回 NULL；输入必须仍然存活。
 * @note 后续解析会覆盖本线程记录，其它线程的解析记录互相独立。
 */
CJSON_PUBLIC(const char *) cJSON_GetErrorPtr(void);

/** @brief 取得 String 节点的 C 文本借用。
 * @param[in] item 稳定节点或 NULL。
 * @return 字符串借用，类型不符或无内容返回 NULL，修改/删除后重新取得。
 */
CJSON_PUBLIC(char *) cJSON_GetStringValue(const cJSON * const item);
/** @brief 读取 Number 节点的双精度值。
 * @param[in] item 稳定节点或 NULL。
 * @return 数值，NULL 或类型不符返回 NaN。
 */
CJSON_PUBLIC(double) cJSON_GetNumberValue(const cJSON * const item);

/** @brief 判断低八位是否为 Invalid。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsInvalid(const cJSON * const item);
/** @brief 判断是否为 False。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsFalse(const cJSON * const item);
/** @brief 判断是否为 True。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsTrue(const cJSON * const item);
/** @brief 判断是否含 True 或 False 类型位。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 含任一布尔位返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsBool(const cJSON * const item);
/** @brief 判断是否为 JSON null。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsNull(const cJSON * const item);
/** @brief 判断是否为 Number。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsNumber(const cJSON * const item);
/** @brief 判断是否为 String。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsString(const cJSON * const item);
/** @brief 判断是否为 Array。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsArray(const cJSON * const item);
/** @brief 判断是否为 Object。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsObject(const cJSON * const item);
/** @brief 判断是否为 Raw 片段。
 * @param[in] item 借用节点，NULL 返回零。
 * @return 匹配返回 1，否则返回 0。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_IsRaw(const cJSON * const item);

/** @brief 创建独占 JSON null 节点。
 * @return 独占节点，分配失败返回 NULL，用 Delete 归还。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNull(void);
/** @brief 创建独占 True 节点。
 * @return 独占节点或 NULL，valueint 保留零初始化，不作为布尔读取接口。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateTrue(void);
/** @brief 创建独占 False 节点。
 * @return 独占节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFalse(void);
/** @brief 由真假值创建独占布尔节点。
 * @param[in] boolean 非零映射 True，零映射 False。
 * @return 独占节点，分配失败 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateBool(cJSON_bool boolean);
/** @brief 创建 Number 并初始化饱和整数缓存。
 * @param[in] num 双精度值，NaN 的整数缓存为零，无穷按边界饱和。
 * @return 独占节点或 NULL，打印 NaN/无穷时输出 null。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateNumber(double num);
/** @brief 深拷贝 C 文本创建 String。
 * @param[in] string NUL 终止借用输入，NULL 失败。
 * @return 独占节点及字符串，任一分配失败均返回 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateString(const char *string);
/** @brief 拷贝原始 JSON 片段，打印时原样输出。
 * @param[in] raw NUL 终止片段，NULL 失败，调用者负责语法。
 * @return 独占节点或 NULL，不执行 JSON 校验。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateRaw(const char *raw);
/** @brief 创建独占空数组。
 * @return 独占节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArray(void);
/** @brief 创建独占空对象。
 * @return 独占节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObject(void);

/** @brief 创建借用外部文本的 String 节点。
 * @param[in] string 借用 NUL 文本，可为 NULL，须覆盖节点使用期且保持稳定。
 * @return 独占包装节点或 NULL，Delete 保留被引用文本。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringReference(const char *string);
/** @brief 创建借用成员链的 Object 包装。
 * @param[in] child 成员链首，可为 NULL，链及键值须覆盖包装使用期。
 * @return 独占包装或 NULL，Delete 保留被引用成员链。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateObjectReference(const cJSON *child);
/** @brief 创建借用元素链的 Array 包装。
 * @param[in] child 元素链首，可为 NULL，须覆盖包装使用期且保持稳定。
 * @return 独占包装或 NULL，Delete 保留被引用链。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateArrayReference(const cJSON *child);

/** @brief 将 int 数组转换为独占 JSON 数组。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败，即使 count 为零。
 * @param[in] count 元素数，负值失败。
 * @return 完整独占数组或 NULL，失败释放全部已建节点。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateIntArray(const int *numbers, int count);
/** @brief 将 float 数组转换为 Number 数组。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败。
 * @param[in] count 元素数，零生成空数组，负值失败。
 * @return 完整独占数组或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateFloatArray(const float *numbers, int count);
/** @brief 将 double 数组转换为 Number 数组。
 * @param[in] numbers 至少含 count 项的借用数组，NULL 失败。
 * @param[in] count 元素数，零生成空数组，负值失败。
 * @return 完整独占数组或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateDoubleArray(const double *numbers, int count);
/** @brief 深拷贝字符串数组为独占 JSON 数组。
 * @param[in] strings 至少含 count 项的借用指针数组，数组或任一文本为 NULL 失败。
 * @param[in] count 元素数，零生成空数组，负值失败。
 * @return 完整独占数组或 NULL，失败清理此前所有元素。
 */
CJSON_PUBLIC(cJSON *) cJSON_CreateStringArray(const char *const *strings, int count);

/** @brief 把独立节点追加到 child 链，成功转交拥有权。
 * @param[in,out] array 可修改父节点，NULL 失败，不强制基础类型。
 * @param[in,out] item 独占且 next/prev 为 NULL 的节点，不得包含父节点。
 * @return 成功 1，参数或已归属链的节点返回 0，失败由调用者继续拥有 item。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToArray(cJSON *array, cJSON *item);
/** @brief 拷贝成员键并追加独立节点到对象成员链。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 调用期间借用键，NULL 失败，可借自 item 的原键。
 * @param[in,out] item 独占未挂接节点，不得包含父节点，成功归 object。
 * @return 成功 1；参数或分配失败 0，失败保留 item 和原树。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
/** @brief 借用长寿命键并追加独立节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 稳定只读键，须覆盖成员使用期，不得借自将被归还的原键。
 * @param[in,out] item 独占未挂接节点，成功归 object。
 * @return 成功 1，失败 0 并保留原树与 item 的拥有权。
 * @note StringIsConst 键不可直接写入或释放。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemToObjectCS(cJSON *object, const char *string, cJSON *item);
/** @brief 追加浅引用包装，原节点及其内容仍由原 owner 管理。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] item 稳定借用节点，NULL 失败，被引用内容须覆盖包装使用期。
 * @return 成功 1，失败 0 并清理包装；不得制造遍历时的引用环。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item);
/** @brief 拷贝新键并将借用内容的包装追加到对象。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 新键，调用期间借用，NULL 失败。
 * @param[in] item 稳定借用节点，被引用内容须覆盖包装使用期。
 * @return 成功 1，失败 0，原 item 不转交，失败包装完整清理。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item);

/** @brief 验证归属后从父链分离指定节点，复杂度 O(n)。
 * @param[in,out] parent 可修改父节点，NULL 失败。
 * @param[in,out] item 必须确在 parent 的 child 链中，NULL 无结果。
 * @return 成功返回独占节点且 next/prev 清空；失败 NULL，保留两棵原树。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemViaPointer(cJSON *parent, cJSON * const item);
/** @brief 按下标分离成员，复杂度 O(n)。
 * @param[in,out] array 可修改父节点，NULL 无结果。
 * @param[in] which 零基下标，负值或越界无结果。
 * @return 独占分离节点或 NULL，成功后由调用者 Delete 或重新挂接。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromArray(cJSON *array, int which);
/** @brief 按下标分离并释放成员。
 * @param[in,out] array 可修改父节点，NULL 不操作。
 * @param[in] which 零基下标，负值或越界不操作。
 * @post 拥有型父节点的匹配成员已释放；引用父节点不操作，其它成员保留。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromArray(cJSON *array, int which);
/** @brief 忽略大小写分离首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 无结果。
 * @param[in] string 借用键，NULL 无结果。
 * @return 独占分离节点或 NULL，成功清空其同级指针。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObject(cJSON *object, const char *string);
/** @brief 区分大小写分离首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 无结果。
 * @param[in] string 借用键，NULL 无结果。
 * @return 独占分离节点或 NULL。
 */
CJSON_PUBLIC(cJSON *) cJSON_DetachItemFromObjectCaseSensitive(cJSON *object, const char *string);
/** @brief 忽略大小写分离并释放首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 不操作。
 * @param[in] string 借用键，NULL 不操作。
 * @post 拥有型父节点的匹配成员已释放，找不到或引用父节点时原树保持。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObject(cJSON *object, const char *string);
/** @brief 区分大小写分离并释放首个同名成员。
 * @param[in,out] object 可修改父节点，NULL 不操作。
 * @param[in] string 借用键，NULL 不操作。
 * @post 拥有型父节点的匹配成员已释放，找不到或引用父节点时原树保持。
 */
CJSON_PUBLIC(void) cJSON_DeleteItemFromObjectCaseSensitive(cJSON *object, const char *string);

/** @brief 在指定下标前插入独立节点，越过末尾时追加。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] which 非负零基下标，已有后继成员向后移动。
 * @param[in,out] newitem 独占未挂接节点，不得包含 array，成功转交。
 * @return 成功 1，失败 0，失败仍由调用者拥有 newitem，查找 O(n)。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_InsertItemInArray(cJSON *array, int which, cJSON *newitem);
/** @brief 核对归属后替换成员并释放旧节点，复杂度 O(n)。
 * @param[in,out] parent 可修改父节点，NULL 失败。
 * @param[in,out] item parent 中的旧成员，NULL 或外来节点失败。
 * @param[in,out] replacement 独占未挂接新节点，不得包含 parent；等于 item 时成功且保留。
 * @return 成功 1，失败 0，失败保留原树与 replacement 的拥有权。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemViaPointer(cJSON * const parent, cJSON * const item, cJSON * replacement);
/** @brief 按下标替换并释放旧成员。
 * @param[in,out] array 可修改父节点，NULL 失败。
 * @param[in] which 零基下标，负值或越界失败。
 * @param[in,out] newitem 独占未挂接替代节点，成功归父节点；可等于原成员。
 * @return 成功 1，失败 0，失败保留原树和新节点。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem);
/** @brief 忽略大小写查找首成员，复制键后提交替换。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 借用查找键，可借自旧成员或 newitem 的键。
 * @param[in,out] newitem 独占未挂接替代节点，可等于匹配成员，成功转交。
 * @return 成功 1，失败 0，缺键/参数/OOM 不修改原树或 newitem。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObject(cJSON *object,const char *string,cJSON *newitem);
/** @brief 区分大小写查找并复制键后替换首成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] string 借用键，NULL 失败，可借自将被替换节点。
 * @param[in,out] newitem 独占未挂接替代节点，可等于匹配成员。
 * @return 成功 1，失败 0 并保持原树与新节点，复杂度 O(n)。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_ReplaceItemInObjectCaseSensitive(cJSON *object,const char *string,cJSON *newitem);

/** @brief 复制单个节点，按选项复制 child 子树。
 * @param[in] item 稳定借用节点，NULL 失败，不复制其 next 同级链。
 * @param[in] recurse 非零深拷贝子树，零只复制本节点与字符串。
 * @return 独占副本或 NULL，失败清理部分树，返回节点 next/prev 为 NULL。
 * @note 引用内容转为拥有副本，常量键仍借用，源键须覆盖副本使用期。
 */
CJSON_PUBLIC(cJSON *) cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
/** @brief 递归比较稳定树，数组按顺序，对象按键双向查找。
 * @param[in] a 第一棵借用树，NULL/无效类型不等。
 * @param[in] b 第二棵借用树，NULL/无效类型不等。
 * @param[in] case_sensitive 非零区分对象键大小写，零忽略大小写。
 * @return 相等 1，否则 0，数值按相对 DBL_EPSILON 比较。
 * @pre 无 child/next 环，递归深度由调用者栈预算保证；重复键沿首匹配比较。
 */
CJSON_PUBLIC(cJSON_bool) cJSON_Compare(const cJSON * const a, const cJSON * const b, const cJSON_bool case_sensitive);

/** @brief 就地移除字符串外空白、行注释和块注释。
 * @param[in,out] json 独占可写 NUL 文本，NULL 不操作，不可传字符串常量。
 * @post 文本前移并重新 NUL 终止，不分配内存，不承诺校验损坏 JSON。
 */
CJSON_PUBLIC(void) cJSON_Minify(char *json);

/** @brief 创建 null 成员并挂接，失败自动清理新节点。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 调用期间借用键，NULL 失败。
 * @return 成功返回已归父树的借用成员，失败 NULL，原树保持。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNullToObject(cJSON * const object, const char * const name);
/** @brief 创建 True 成员并挂接，失败自动清理。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 借用新成员或 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddTrueToObject(cJSON * const object, const char * const name);
/** @brief 创建 False 成员并挂接，失败自动清理。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 借用新成员或 NULL。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddFalseToObject(cJSON * const object, const char * const name);
/** @brief 创建并挂接布尔成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] boolean 非零为 True，零为 False。
 * @return 借用新成员或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddBoolToObject(cJSON * const object, const char * const name, const cJSON_bool boolean);
/** @brief 创建并挂接 Number 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] number 数值，沿 CreateNumber 的 NaN/饱和规则。
 * @return 借用新成员或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddNumberToObject(cJSON * const object, const char * const name, const double number);
/** @brief 复制文本并挂接 String 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] string 借用 C 文本，NULL 失败。
 * @return 借用新成员或 NULL，失败清理全部新分配。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string);
/** @brief 复制原始片段并挂接 Raw 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @param[in] raw 借用 NUL 片段，NULL 失败，调用者保证其语法。
 * @return 借用新成员或 NULL，失败清理新分配。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddRawToObject(cJSON * const object, const char * const name, const char * const raw);
/** @brief 创建并挂接空 Object 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 借用新成员或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddObjectToObject(cJSON * const object, const char * const name);
/** @brief 创建并挂接空 Array 成员。
 * @param[in,out] object 可修改父节点，NULL 失败。
 * @param[in] name 借用键，NULL 失败。
 * @return 借用新成员或 NULL，失败清理新节点。
 */
CJSON_PUBLIC(cJSON*) cJSON_AddArrayToObject(cJSON * const object, const char * const name);

/** 同时赋值整数缓存与 double；object/number 可重复求值，参数不得带副作用。
 * 超出 int 表示范围的输入应使用 SetNumberValue，NULL 时只返回 number。 */
#define cJSON_SetIntValue(object, number) ((object) ? (object)->valueint = (object)->valuedouble = (number) : (number))
/** @brief 更新 double 与饱和整数缓存，不改变基础类型。
 * @param[in,out] object 可修改节点，NULL 返回 NaN。
 * @param[in] number 新值，NaN 缓存零，无穷/超界按 int 边界饱和。
 * @return number，object 为 NULL 时返回 NaN。
 */
CJSON_PUBLIC(double) cJSON_SetNumberHelper(cJSON *object, double number);
/** 更新数字的宏，object 可重复求值，NULL 时直接返回 number。 */
#define cJSON_SetNumberValue(object, number) ((object != NULL) ? cJSON_SetNumberHelper(object, (double)number) : (number))
/** @brief 更新拥有型 String 内容，容量足够时就地移动，否则先复制再替换。
 * @param[in,out] object 可修改拥有型 String，NULL、引用或缺少旧文本均失败。
 * @param[in] valuestring 新 C 文本，可借自旧值或旧值内部子串，NULL 失败。
 * @return 成功返回节点的新文本借用，失败 NULL 并保留旧值。
 */
CJSON_PUBLIC(char*) cJSON_SetValuestring(cJSON *object, const char *valuestring);

/** 改写布尔类型位并返回新组合类型，NULL/非布尔返回 Invalid。
 * 不更新数字缓存，object 可能重复求值，参数不得带副作用。 */
#define cJSON_SetBoolValue(object, boolValue) ( \
    (object != NULL && ((object)->type & (cJSON_False|cJSON_True))) ? \
    (object)->type=((object)->type &(~(cJSON_False|cJSON_True)))|((boolValue)?cJSON_True:cJSON_False) : \
    cJSON_Invalid\
)

/** 借用遍历 child 同级链，array 可为 NULL；循环中删除当前项前先保存 next。 */
#define cJSON_ArrayForEach(element, array) for(element = (array != NULL) ? (array)->child : NULL; element != NULL; element = element->next)

/** @brief 用当前共享钩子分配独占内存。
 * @param[in] size 请求字节数，零沿分配器行为。
 * @return 独占地址或 NULL，须在相容钩子仍生效时用 cJSON_free 归还。
 */
CJSON_PUBLIC(void *) cJSON_malloc(size_t size);
/** @brief 通过当前钩子释放独占地址。
 * @param[in] object 配对分配地址或 NULL，调用不清除调用者的指针槽。
 * @post 分配已归还，调用者不得继续访问。
 */
CJSON_PUBLIC(void) cJSON_free(void *object);

#ifdef __cplusplus
}
#endif

#endif

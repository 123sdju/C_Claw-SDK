



#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_fields.h"
#include "cc/internal/cc_alloc.h"
#include "cJSON.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 在已解析区间内检查字符串的 Unicode 转义均由四个十六进制字符组成。
 * @param[in] text 借用的输入起点，须与 end 属于同一有效文本缓冲。
 * @param[in] end cJSON 返回的首个已解析值的结束位置，区间为左闭右开。
 * @return 已消费区间中的 Unicode 转义均合法为一，字符缺失或非十六进制为零。
 * @note 无动态分配，按引号及反斜杠转义推进，字面双反斜杠后的 u 文本保持原样。
 */
static int json_unicode_escapes_valid(const char *text, const char *end)
{
    int in_string = 0;
    for (const char *cursor = text; cursor < end; cursor++) {
        if (*cursor == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string || *cursor != '\\') continue;
        cursor++;
        if (cursor == end) return 0;
        if (*cursor != 'u') continue;
        for (int digit = 0; digit < 4; digit++) {
            cursor++;
            if (cursor == end || !((*cursor >= '0' && *cursor <= '9') ||
                (*cursor >= 'a' && *cursor <= 'f') || (*cursor >= 'A' && *cursor <= 'F')))
                return 0;
        }
    }
    return 1;
}

/**
 * @brief 解析首个值后检查已消费原文中的 Unicode 转义，再交付拥有树。
 * @param[in] text 调用期间借用的 NUL 结尾正文，可为 NULL。
 * @param[out] out_value 非 NULL 时先置空，成功取得拥有树，调用者 cc_json_destroy。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，解析、Unicode 转义或分配失败为 CC_ERR_JSON。
 * 底层解析失败只格式化从零起算的输入字节偏移，诊断分配失败时 message 为 NULL，code 保留。
 * @note 位置由本次 return_parse_end 取得，仅在输入借用有效期内求字节差，不复制输入尾部。
 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value)
{
    if (!out_value) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON output");
    *out_value = NULL;
    if (!text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON text");
    const char *parse_end = NULL;
    cJSON *val = cJSON_ParseWithOpts(text, &parse_end, 0);
    if (!val) {
        size_t offset = parse_end ? (size_t)(parse_end - text) : 0;
        return cc_result_errf(CC_ERR_JSON, "JSON parse error at byte %zu", offset);
    }
    if (!json_unicode_escapes_valid(text, parse_end)) {
        cJSON_Delete(val);
        return cc_result_error(CC_ERR_JSON, "JSON Unicode escape is invalid");
    }
    *out_value = (cc_json_value_t *)val;
    return cc_result_ok();
}

/**
 * @brief 要求 cJSON 完整消费尾部，并在交付前检查已消费原文中的 Unicode 转义。
 * @param[in] text 借用的 NUL 结尾正文，可为 NULL。
 * @param[out] out_value 两个参数均非 NULL 后先置空，成功输出拥有树，调用者 cc_json_destroy。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，解析、Unicode 转义、尾部校验或分配失败统一为 CC_ERR_JSON。
 * 底层失败诊断仅含固定说明和零基字节偏移，诊断分配失败时 message 为 NULL，code 保留。
 * @note 数字、字符串和空白处理沿用当前 cJSON，重复键及业务字段约束由调用层校验，具体范围见公共接口。
 */
cc_result_t cc_json_parse_strict(const char *text, cc_json_value_t **out_value)
{
    if (!out_value || !text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid JSON input");
    *out_value = NULL;
    const char *parse_end = NULL;
    cJSON *value = cJSON_ParseWithOpts(text, &parse_end, 1);
    if (!value) {
        size_t offset = parse_end ? (size_t)(parse_end - text) : 0;
        return cc_result_errf(CC_ERR_JSON, "JSON is invalid or has trailing data at byte %zu", offset);
    }
    if (!json_unicode_escapes_valid(text, parse_end)) {
        cJSON_Delete(value);
        return cc_result_error(CC_ERR_JSON, "JSON Unicode escape is invalid");
    }
    *out_value = (cc_json_value_t *)value;
    return cc_result_ok();
}


/**
 * @brief 使用 cJSON_Print 排版后复制正文，再通过 cJSON_free 释放打印缓冲。
 * @param[in] value 借用节点，NULL 时直接复制 "null"。
 * @return 可由调用者 free 的拥有字符串，打印或复制失败返回 NULL。
 */
char *cc_json_stringify(const cc_json_value_t *value)
{
    if (!value) return cc_copy_string("null");
    char *printed = cJSON_Print((cJSON *)value);
    if (!printed) return NULL;
    char *result = cc_copy_string(printed);
    cJSON_free(printed);
    return result;
}


/**
 * @brief 使用 cJSON_PrintUnformatted 生成紧凑正文，复制后释放库内打印缓冲。
 * @param[in] value 借用节点，NULL 时直接复制 "null"。
 * @return 可由调用者 free 的拥有字符串，打印或复制失败返回 NULL。
 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value)
{
    if (!value) return cc_copy_string("null");
    char *printed = cJSON_PrintUnformatted((cJSON *)value);
    if (!printed) return NULL;
    char *result = cc_copy_string(printed);
    cJSON_free(printed);
    return result;
}


/**
 * @brief 将拥有的根及其子树交给 cJSON_Delete 释放。
 * @param[in,out] value 独立拥有的根，可为 NULL，已挂接节点随父树销毁。
 * @post 根及子树的拥有内存已释放，原参数值由调用者自行清空。
 */
void cc_json_destroy(cc_json_value_t *value)
{
    if (value) cJSON_Delete((cJSON *)value);
}


/**
 * @brief 通过 cJSON_GetObjectItem 查找忽略大小写的首个匹配字段。
 * @param[in] obj 借用对象，NULL 时返回 NULL。
 * @param[in] key 借用字段名，NULL 时返回 NULL。
 * @return 匹配子节点的借用指针，缺失为 NULL，父树销毁或该项替换后结束寿命。
 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return (cc_json_value_t *)cJSON_GetObjectItem((cJSON *)obj, key);
}

/**
 * @brief 按字段原始大小写查找首个匹配子节点。
 * @param[in] obj 调用期间借用的对象，NULL 时返回 NULL。
 * @param[in] key 借用字段名，NULL 时返回 NULL。
 * @return 匹配子节点的借用指针，缺失返回 NULL，寿命受父树及字段替换约束。
 */
cc_json_value_t *cc_json_object_get_case_sensitive(const cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return (cc_json_value_t *)cJSON_GetObjectItemCaseSensitive((const cJSON *)obj, key);
}

/**
 * @brief 类型检查后遍历 child 链表，统计对象字段。
 * @param[in] obj 借用节点，可为 NULL。
 * @return 字段数，NULL 或非对象返回零。
 */
int cc_json_object_size(const cc_json_value_t *obj)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj)) return 0;
    int count = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next) count++;
    return count;
}

/**
 * @brief 线性遍历对象 child 链表，读取指定位置的 string 字段名。
 * @param[in] obj 借用节点，可为 NULL，须为对象才有结果。
 * @param[in] index 从零开始的字段下标，负数或越界返回 NULL。
 * @return 借用字段名，节点或下标无效为 NULL，寿命受父树及该项替换约束。
 */
const char *cc_json_object_key_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return child->string;
    }
    return NULL;
}

/**
 * @brief 线性遍历对象 child 链表，返回指定位置的节点。
 * @param[in] obj 借用节点，可为 NULL，须为对象才有结果。
 * @param[in] index 从零开始的字段下标，负数或越界返回 NULL。
 * @return 借用子节点，无有效项为 NULL，父树销毁或该项替换后结束寿命。
 */
cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return (cc_json_value_t *)child;
    }
    return NULL;
}


/**
 * @brief 类型检查后读取 cJSON 内部字符串缓冲。
 * @param[in] value 借用节点，可为 NULL。
 * @return 借用正文，NULL 或非字符串为 NULL，随该节点销毁结束寿命。
 */
const char *cc_json_string_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsString((cJSON *)value)) return NULL;
    return cJSON_GetStringValue((cJSON *)value);
}


/**
 * @brief 类型检查后直接读取 valueint 缓存。
 * @param[in] value 借用节点，可为 NULL。
 * @return 数字的 int 视图，NULL 或非数字为零，缓存的截断和饱和规则由 cJSON 生成数字时决定。
 */
int cc_json_int_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0;
    return ((cJSON *)value)->valueint;
}


/**
 * @brief 类型检查后直接读取 valuedouble。
 * @param[in] value 借用节点，可为 NULL。
 * @return 数字的 double 视图，NULL 或非数字为 0.0，范围和有限性检查由调用者完成。
 */
double cc_json_number_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0.0;
    return ((cJSON *)value)->valuedouble;
}


/**
 * @brief 使用 cJSON_IsTrue 读取布尔真值。
 * @param[in] value 借用节点，可为 NULL。
 * @return JSON true 为一，false、其他类型或 NULL 为零。
 */
int cc_json_bool_value(const cc_json_value_t *value)
{
    if (!value) return 0;
    return cJSON_IsTrue((cJSON *)value) ? 1 : 0;
}


/**
 * @brief 检查 cJSON 对象类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return 对象为一，其他类型或 NULL 为零。
 */
int cc_json_is_object(const cc_json_value_t *value)
{
    return value && cJSON_IsObject((cJSON *)value);
}


/**
 * @brief 检查 cJSON 数组类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return 数组为一，其他类型或 NULL 为零。
 */
int cc_json_is_array(const cc_json_value_t *value)
{
    return value && cJSON_IsArray((cJSON *)value);
}


/**
 * @brief 检查 cJSON 字符串类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return 字符串为一，其他类型或 NULL 为零。
 */
int cc_json_is_string(const cc_json_value_t *value)
{
    return value && cJSON_IsString((cJSON *)value);
}


/**
 * @brief 检查 cJSON 数字类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return 数字为一，其他类型或 NULL 为零。
 */
int cc_json_is_number(const cc_json_value_t *value)
{
    return value && cJSON_IsNumber((cJSON *)value);
}


/**
 * @brief 检查 cJSON 布尔类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return true 或 false 节点为一，其他类型或 NULL 为零。
 */
int cc_json_is_bool(const cc_json_value_t *value)
{
    return value && (cJSON_IsBool((cJSON *)value));
}


/**
 * @brief 检查 cJSON 空值节点类型。
 * @param[in] value 借用节点，可为 NULL。
 * @return JSON null 节点为一，其他类型或 C 空指针为零。
 */
int cc_json_is_null(const cc_json_value_t *value)
{
    return value && cJSON_IsNull((cJSON *)value);
}


/**
 * @brief 调用 cJSON_CreateObject 创建空对象。
 * @return 拥有节点，由调用者销毁或成功交给父节点，分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_object(void)
{
    return (cc_json_value_t *)cJSON_CreateObject();
}


/**
 * @brief 调用 cJSON_CreateArray 创建空数组。
 * @return 拥有节点，由调用者销毁或成功交给父节点，分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_array(void)
{
    return (cc_json_value_t *)cJSON_CreateArray();
}


/**
 * @brief 将 NULL 正文归一为空字符串后交给 cJSON_CreateString 复制。
 * @param[in] value 借用的 NUL 结尾正文，可为 NULL。
 * @return 拥有节点，由调用者销毁或成功交给父节点，节点或正文分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_string(const char *value)
{
    return (cc_json_value_t *)cJSON_CreateString(value ? value : "");
}


/**
 * @brief 以安全值建立整数缓存后保存原始 double，保留 NaN 与无穷大的读取及打印语义。
 * @param[in] value 待保存数值，NaN 的 int 缓存为零，无穷大与超范围有限值的 int 缓存按上下界饱和。
 * @return 拥有节点，由调用者销毁或成功交给父节点，分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_number(double value)
{
    cJSON *number = cJSON_CreateNumber(isnan(value) ? 0.0 : value);
    if (number) number->valuedouble = value;
    return (cc_json_value_t *)number;
}


/**
 * @brief 调用 cJSON_CreateBool 创建布尔节点。
 * @param[in] value 零生成 false，非零生成 true。
 * @return 拥有节点，由调用者销毁或成功交给父节点，分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_bool(int value)
{
    return (cc_json_value_t *)cJSON_CreateBool(value);
}


/**
 * @brief 调用 cJSON_CreateNull 创建 JSON 空值节点。
 * @return 拥有节点，由调用者销毁或成功交给父节点，分配失败为 NULL。
 */
cc_json_value_t *cc_json_create_null(void)
{
    return (cc_json_value_t *)cJSON_CreateNull();
}


/**
 * @brief 在保留失败拥有权的前提下，返回底层字段替换或追加结果。
 * @param[in,out] obj 接收字段的借用对象，NULL 时返回零。
 * @param[in] key 借用字段名，NULL 时返回零，底层在替换原项前复制名称。
 * @param[in,out] value 待转移独占节点，NULL 时返回零，成功归父对象，失败仍归调用者。
 * @return 底层挂接成功为一，失败为零，返回后结束对 key 的访问。
 */
static int json_object_set_impl(cc_json_value_t *obj, const char *key, cc_json_value_t *value)
{
    if (!obj || !key || !value) return 0;
    cJSON *object = (cJSON *)obj;
    if (cJSON_GetObjectItemCaseSensitive(object, key))
        return cJSON_ReplaceItemInObjectCaseSensitive(object, key, (cJSON *)value);
    return cJSON_AddItemToObject(object, key, (cJSON *)value);
}

/**
 * @brief 用区分大小写的查找选择替换或追加，将字段操作交给 cJSON。
 * @pre obj 为对象，value 为独立拥有且尚未挂接的节点，两者须无自引用或祖先关系。
 * @param[in,out] obj 接收节点的对象，NULL 时直接返回。
 * @param[in] key 借用的字段名，cJSON 复制名称，NULL 时直接返回。
 * @param[in,out] value 待转移节点，NULL 时直接返回，成功时归父对象拥有。
 * @post 挂接成功后原同名项已销毁，失败时 value 仍归调用者。底层布尔结果在此入口被丢弃，调用者须核对挂接结果。
 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value)
{
    (void)json_object_set_impl(obj, key, value);
}

/**
 * @brief 挂接一个独占字段，并在失败时销毁尚未交接的节点。
 * @pre 非空 obj 为对象，value 为独占且尚未挂接的节点，与 obj 无自引用或祖先关系。
 * @param[in,out] obj 接收字段的借用对象，NULL 时仍消费 value。
 * @param[in] key 调用期间借用的字段名，NULL 时仍消费 value。
 * @param[in,out] value 消费的独占节点，允许 NULL，成功归父对象，失败由本函数销毁。
 * @return 底层区分大小写的挂接操作成功为一，其他为零，value 的原拥有权均已交出。
 */
int cc_json_object_set_owned(cc_json_value_t *obj, const char *key, cc_json_value_t *value)
{
    if (json_object_set_impl(obj, key, value)) return 1;
    cc_json_destroy(value);
    return 0;
}


/**
 * @brief 参数非空时调用 cJSON_AddItemToArray，将节点追加到尾部。
 * @pre arr 为数组，value 为独立拥有且尚未挂接的节点，两者须无自引用或祖先关系。
 * @param[in,out] arr 接收节点的数组，NULL 时直接返回。
 * @param[in,out] value 待转移节点，NULL 时直接返回。
 * @post 成功时由 arr 拥有 value，空参数或底层拒绝时保留调用者所有权，函数以 void 返回。
 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value)
{
    if (arr && value)
        cJSON_AddItemToArray((cJSON *)arr, (cJSON *)value);
}

/**
 * @brief 消费独占节点，将其追加到有效数组，失败时销毁该节点。
 * @pre value 为独占且尚未挂接的节点，与 arr 无自引用或祖先关系。
 * @param[in,out] arr 借用接收数组，NULL 或其他类型时仍消费 value。
 * @param[in,out] value 消费的节点，允许 NULL，成功归数组，失败在此销毁。
 * @return 底层追加成功为一，参数或类型无效、底层拒绝时为零，原拥有权均已交出。
 */
int cc_json_array_append_owned(cc_json_value_t *arr, cc_json_value_t *value)
{
    if (arr && value && cJSON_IsArray((cJSON *)arr) &&
        cJSON_AddItemToArray((cJSON *)arr, (cJSON *)value)) return 1;
    cc_json_destroy(value);
    return 0;
}

/**
 * @brief 复用 cJSON 字段查找语义，检查每个已知字段在首项之后是否再次出现。
 * @param[in] object 借用的有效稳定对象，NULL 或非对象时返回 NULL。
 * @param[in] fields 借用字段表及字符串，count 非零时须非 NULL，各名称互不重复。
 * @param[in] count 字段数量，零时直接返回 NULL。
 * @return 首个重复字段对应的 fields 项借用，无重复或无有效对象时 NULL。
 * @note 栈上 tail 只借用首匹配项之后的链表，查找不改写节点，也不销毁该借用视图。
 * 无分配及对象成员数量限制，整个调用期间由外层排除树的修改或销毁。
 */
const char *cc_json_duplicate_known_field(
    const cc_json_value_t *object,
    const char *const *fields,
    size_t count)
{
    if (!cc_json_is_object(object) || (count && !fields)) return NULL;
    for (size_t index = 0; index < count; index++) {
        const cJSON *first = cJSON_GetObjectItem((const cJSON *)object, fields[index]);
        if (!first) continue;
        cJSON tail = {0};
        tail.type = cJSON_Object;
        tail.child = first->next;
        if (cJSON_GetObjectItem(&tail, fields[index])) return fields[index];
    }
    return NULL;
}


/**
 * @brief 类型检查后调用 cJSON_GetArraySize 统计数组元素。
 * @param[in] arr 借用节点，可为 NULL。
 * @return 元素数，NULL 或非数组为零。
 */
int cc_json_array_size(const cc_json_value_t *arr)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return 0;
    return cJSON_GetArraySize((cJSON *)arr);
}


/**
 * @brief 类型检查后通过 cJSON_GetArrayItem 按下标读取元素。
 * @param[in] arr 借用节点，可为 NULL，须为数组才有结果。
 * @param[in] index 从零开始的下标，负数或越界返回 NULL。
 * @return 借用元素，无有效项为 NULL，数组修改或销毁后调用者需重新获取。
 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return NULL;
    return (cc_json_value_t *)cJSON_GetArrayItem((cJSON *)arr, index);
}

/**
 * @brief 数组类型检查后直接读取 child，开始链表顺序遍历。
 * @param[in] arr 借用节点，可为 NULL。
 * @return 借用首元素，NULL、非数组或空数组为 NULL，遍历期间父数组须保持有效且稳定。
 */
const cc_json_value_t *cc_json_array_first(const cc_json_value_t *arr)
{
    return arr && cJSON_IsArray((const cJSON *)arr) ? ((const cJSON *)arr)->child : NULL;
}

/**
 * @brief 直接读取元素的 next，按常数开销推进迭代。
 * @param[in] item first/next 返回的借用数组元素，可为 NULL，父数组须保持有效且稳定。
 * @return 借用后继元素，输入 NULL 或到达尾部返回 NULL。
 */
const cc_json_value_t *cc_json_array_next(const cc_json_value_t *item)
{
    return item ? ((const cJSON *)item)->next : NULL;
}





#include "cc/util/cc_json.h"
#include "cc/internal/cc_alloc.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

/*
 * 从字符串解析 JSON。
 *
 * 这里把 cJSON 指针隐藏为 cc_json_value_t，避免 public API 暴露 vendor 类型。解析失败时
 * 返回 cJSON 提供的错误位置，便于配置和 provider 响应定位问题。
 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value)
{
    if (!out_value) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON output");
    *out_value = NULL;
    if (!text) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null JSON text");
    cJSON *val = cJSON_Parse(text);
    if (!val) {
        const char *err = cJSON_GetErrorPtr();
        return cc_result_error(CC_ERR_JSON, err ? err : "JSON parse error");
    }
    *out_value = (cc_json_value_t *)val;
    return cc_result_ok();
}


/* 格式化输出 JSON；NULL value 按 JSON null 处理，返回字符串由调用方 free()。 */
char *cc_json_stringify(const cc_json_value_t *value)
{
    if (!value) return cc_copy_string("null");
    char *printed = cJSON_Print((cJSON *)value);
    if (!printed) return NULL;
    char *result = cc_copy_string(printed);
    cJSON_free(printed);
    return result;
}


/* 输出紧凑 JSON；常用于 provider payload、event payload 和测试 golden。 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value)
{
    if (!value) return cc_copy_string("null");
    char *printed = cJSON_PrintUnformatted((cJSON *)value);
    if (!printed) return NULL;
    char *result = cc_copy_string(printed);
    cJSON_free(printed);
    return result;
}


/* 销毁 JSON AST；cJSON 会递归释放子节点。 */
void cc_json_destroy(cc_json_value_t *value)
{
    if (value) cJSON_Delete((cJSON *)value);
}


/* 按 key 获取 object 子节点；返回借用指针，不转移所有权。 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key)
{
    if (!obj || !key) return NULL;
    return (cc_json_value_t *)cJSON_GetObjectItem((cJSON *)obj, key);
}

/* 统计 object 子节点数量；cJSON 没有直接 object size API，因此遍历 child 链表。 */
int cc_json_object_size(const cc_json_value_t *obj)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj)) return 0;
    int count = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next) count++;
    return count;
}

/* 按索引返回 object key；用于 redaction 等需要遍历未知字段的路径。 */
const char *cc_json_object_key_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return child->string;
    }
    return NULL;
}

/* 按索引返回 object value；返回借用指针。 */
cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index)
{
    if (!obj || !cJSON_IsObject((cJSON *)obj) || index < 0) return NULL;
    int i = 0;
    for (cJSON *child = ((cJSON *)obj)->child; child; child = child->next, i++) {
        if (i == index) return (cc_json_value_t *)child;
    }
    return NULL;
}


/* 读取字符串节点值；返回 cJSON 内部指针，不能释放。 */
const char *cc_json_string_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsString((cJSON *)value)) return NULL;
    return cJSON_GetStringValue((cJSON *)value);
}


/* 读取 number 的整数视图；非 number 返回 0。 */
int cc_json_int_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0;
    return ((cJSON *)value)->valueint;
}


/* 读取 number 的 double 视图；非 number 返回 0.0。 */
double cc_json_number_value(const cc_json_value_t *value)
{
    if (!value || !cJSON_IsNumber((cJSON *)value)) return 0.0;
    return ((cJSON *)value)->valuedouble;
}


/* 读取 bool；只有 JSON true 返回 1，false/其他类型返回 0。 */
int cc_json_bool_value(const cc_json_value_t *value)
{
    if (!value) return 0;
    return cJSON_IsTrue((cJSON *)value) ? 1 : 0;
}


/* 判断节点是否为 object。 */
int cc_json_is_object(const cc_json_value_t *value)
{
    return value && cJSON_IsObject((cJSON *)value);
}


/* 判断节点是否为 array。 */
int cc_json_is_array(const cc_json_value_t *value)
{
    return value && cJSON_IsArray((cJSON *)value);
}


/* 判断节点是否为 string。 */
int cc_json_is_string(const cc_json_value_t *value)
{
    return value && cJSON_IsString((cJSON *)value);
}


/* 判断节点是否为 number。 */
int cc_json_is_number(const cc_json_value_t *value)
{
    return value && cJSON_IsNumber((cJSON *)value);
}


/* 判断节点是否为 bool。 */
int cc_json_is_bool(const cc_json_value_t *value)
{
    return value && (cJSON_IsBool((cJSON *)value));
}


/* 判断节点是否为 null。 */
int cc_json_is_null(const cc_json_value_t *value)
{
    return value && cJSON_IsNull((cJSON *)value);
}


/* 创建空 object 节点，调用方或父节点负责释放。 */
cc_json_value_t *cc_json_create_object(void)
{
    return (cc_json_value_t *)cJSON_CreateObject();
}


/* 创建空 array 节点。 */
cc_json_value_t *cc_json_create_array(void)
{
    return (cc_json_value_t *)cJSON_CreateArray();
}


/* 创建 string 节点；NULL 输入按空字符串处理。 */
cc_json_value_t *cc_json_create_string(const char *value)
{
    return (cc_json_value_t *)cJSON_CreateString(value ? value : "");
}


/* 创建 number 节点。 */
cc_json_value_t *cc_json_create_number(double value)
{
    return (cc_json_value_t *)cJSON_CreateNumber(value);
}


/* 创建 bool 节点。 */
cc_json_value_t *cc_json_create_bool(int value)
{
    return (cc_json_value_t *)cJSON_CreateBool(value);
}


/* 创建 null 节点。 */
cc_json_value_t *cc_json_create_null(void)
{
    return (cc_json_value_t *)cJSON_CreateNull();
}


/*
 * 设置 object 字段。
 *
 * 如果字段已存在则替换旧节点，否则新增字段；成功后 object 接管 value 所有权。无效参数
 * 直接返回，调用方若传入 value 后失败不会被释放，因此通常应先校验参数。
 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value)
{
    if (!obj || !key || !value) return;
    cJSON *object = (cJSON *)obj;
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        cJSON_ReplaceItemInObjectCaseSensitive(object, key, (cJSON *)value);
    } else {
        cJSON_AddItemToObject(object, key, (cJSON *)value);
    }
}


/* 追加 array 元素；成功后 array 接管 value 所有权。 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value)
{
    if (arr && value)
        cJSON_AddItemToArray((cJSON *)arr, (cJSON *)value);
}


/* 返回 array 元素数量；非 array 返回 0。 */
int cc_json_array_size(const cc_json_value_t *arr)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return 0;
    return cJSON_GetArraySize((cJSON *)arr);
}


/* 按索引获取 array 元素；返回借用指针。 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index)
{
    if (!arr || !cJSON_IsArray((cJSON *)arr)) return NULL;
    return (cc_json_value_t *)cJSON_GetArrayItem((cJSON *)arr, index);
}

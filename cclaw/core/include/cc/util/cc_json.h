



#ifndef CC_JSON_H
#define CC_JSON_H

#include "cc/core/cc_result.h"

/*
 * JSON AST 不透明类型。
 *
 * 当前实现可以基于 vendor JSON 库，但 public API 不暴露具体结构，避免下游依赖第三方
 * 类型。所有 create/parse 返回的 value 都通过 cc_json_destroy() 释放。
 */
typedef void cc_json_value_t;

/* 从字符串解析 JSON；out_value 成功后由调用方 cc_json_destroy()。 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value);

/* 格式化输出 JSON；返回字符串由调用方 free()。 */
char *cc_json_stringify(const cc_json_value_t *value);

/* 输出无额外空白的 JSON；适合 provider payload 和事件 payload。 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value);

/* 销毁 JSON AST；允许 NULL。 */
void cc_json_destroy(cc_json_value_t *value);

/* 获取对象字段；返回借用指针，生命周期不超过 obj。 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key);

/* 返回对象字段数；非对象返回 0。 */
int cc_json_object_size(const cc_json_value_t *obj);

/* 按索引读取对象 key；返回借用字符串。 */
const char *cc_json_object_key_at(const cc_json_value_t *obj, int index);

/* 按索引读取对象 value；返回借用指针。 */
cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index);

/* 读取字符串值；非字符串返回 NULL。 */
const char *cc_json_string_value(const cc_json_value_t *value);

/* 读取整数值；非数字按 0 处理。 */
int cc_json_int_value(const cc_json_value_t *value);

/* 读取浮点数值；非数字按 0 处理。 */
double cc_json_number_value(const cc_json_value_t *value);

/* 读取布尔值；非布尔按 0 处理。 */
int cc_json_bool_value(const cc_json_value_t *value);



/* 类型判断 helpers；返回 1 表示匹配，0 表示不匹配或 NULL。 */
int cc_json_is_object(const cc_json_value_t *value);


/* 判断 JSON 值是否为数组类型；NULL 值返回 0。 */
int cc_json_is_array(const cc_json_value_t *value);


/* 判断 JSON 值是否为字符串类型；NULL 值返回 0。 */
int cc_json_is_string(const cc_json_value_t *value);


/* 判断 JSON 值是否为数字类型；NULL 值返回 0。 */
int cc_json_is_number(const cc_json_value_t *value);


/* 判断 JSON 值是否为布尔类型；NULL 值返回 0。 */
int cc_json_is_bool(const cc_json_value_t *value);


/* 判断 JSON 值是否为 null 类型；NULL 值返回 0。 */
int cc_json_is_null(const cc_json_value_t *value);



/* 创建空 object；返回值由调用方或父节点接管后统一 destroy。 */
cc_json_value_t *cc_json_create_object(void);

/* 创建空 array。 */
cc_json_value_t *cc_json_create_array(void);

/* 创建字符串节点；value 会被 JSON 库复制，NULL 通常按空字符串处理。 */
cc_json_value_t *cc_json_create_string(const char *value);

/* 创建 number 节点。 */
cc_json_value_t *cc_json_create_number(double value);

/* 创建 bool 节点；value 非 0 表示 true。 */
cc_json_value_t *cc_json_create_bool(int value);

/* 创建 null 节点。 */
cc_json_value_t *cc_json_create_null(void);



/* 把 value 挂到 object key；成功后 obj 接管 value 所有权。 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value);

/* 把 value 追加到 array；成功后 arr 接管 value 所有权。 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value);

/* 返回 array 长度；非数组返回 0。 */
int cc_json_array_size(const cc_json_value_t *arr);

/* 按索引读取 array 元素；返回借用指针。 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index);

#endif

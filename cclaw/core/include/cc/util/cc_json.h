



#ifndef CC_JSON_H
#define CC_JSON_H

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JSON 树的不透明节点类型，当前由 cJSON 实现。
 *
 * create/parse 取得的根节点由调用者 cc_json_destroy，挂接成功的子节点由父节点统一销毁。
 * 查询取得的节点和字符串均为借用，使用期间调用者负责保持树有效并协调并发访问。
 */
typedef void cc_json_value_t;

/**
 * @brief 按 cJSON 规则解析输入开头的一个 JSON 值。
 * @param[in] text 借用的 NUL 结尾正文，NULL 返回参数错误。
 * @param[out] out_value 必需输出槽，非 NULL 时先置空，成功取得拥有树，由调用者 cc_json_destroy。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，解析、Unicode 转义或分配失败统一为 CC_ERR_JSON。
 * @note 本入口允许首个值后的额外内容，完整消费要求使用 cc_json_parse_strict。
 * 首值已消费原文中的 Unicode 转义须含四个十六进制字符，尾部未消费内容继续保留兼容行为。
 * 底层失败消息只含固定说明和从零起算的输入字节偏移，不复制原文；消息分配失败时为 NULL，code 保留。
 */
cc_result_t cc_json_parse(const char *text, cc_json_value_t **out_value);
/**
 * @brief 按 cJSON 规则解析一个值，并要求消费尾部空白后到达 NUL。
 * @param[in] text 借用的 NUL 结尾正文，NULL 返回参数错误。
 * @param[out] out_value 必需输出槽，两个参数均非 NULL 后先置空，成功树由调用者 cc_json_destroy。
 * @return 成功为 CC_OK，NULL 参数为 CC_ERR_INVALID_ARGUMENT，解析、Unicode 转义、尾部内容或分配失败统一为 CC_ERR_JSON。
 * @note 完整消费沿用当前 cJSON 的语法规则，包括开头 BOM、前导零或末尾小数点等宽松数字形式，
 * 以及值前后按字节值小于等于 0x20 识别的空白。解析树可包含重复字段和解码 NUL，原始字符串字节按底层规则接纳。
 * 存储或调用层根据需要追加字段唯一性、控制字节、Unicode 及业务约束校验。
 * 两个解析入口均补充检查已消费原文中 Unicode 转义的四位十六进制形式，合法 u0000 仍可解码为 NUL。
 * 底层失败消息只含固定说明和零基字节偏移，UTF-8 按字节计数，诊断分配失败保留 code 并置空 message。
 */
cc_result_t cc_json_parse_strict(const char *text, cc_json_value_t **out_value);

/**
 * @brief 将节点序列化为带排版空白的 JSON 字符串。
 * @param[in] value 借用的节点，NULL 序列化为 "null"。
 * @return 拥有的字符串，由调用者 free，打印或复制分配失败返回 NULL。
 */
char *cc_json_stringify(const cc_json_value_t *value);

/**
 * @brief 将节点序列化为紧凑 JSON 字符串。
 * @param[in] value 借用的节点，NULL 序列化为 "null"。
 * @return 拥有的字符串，由调用者 free，打印或复制分配失败返回 NULL。
 */
char *cc_json_stringify_unformatted(const cc_json_value_t *value);

/**
 * @brief 销毁拥有的 JSON 根节点及其子树。
 * @param[in,out] value 独立拥有的根节点，可为 NULL，已挂接子节点由父节点管理。
 * @post 非 NULL 根及子树已释放，所有相关借用节点和字符串结束寿命，调用者需自行清空原指针。
 */
void cc_json_destroy(cc_json_value_t *value);

/**
 * @brief 按忽略大小写的字段名查找首个匹配子节点。
 * @param[in] obj 借用的对象，NULL 返回 NULL。
 * @param[in] key 借用的字段名，NULL 返回 NULL。
 * @return 匹配的借用节点，缺失返回 NULL，生命周期受父树及该字段的替换操作约束。
 */
cc_json_value_t *cc_json_object_get(const cc_json_value_t *obj, const char *key);

/**
 * @brief 按区分大小写的字段名查找首个匹配节点。
 * @param[in] obj 借用对象，NULL 时返回 NULL。
 * @param[in] key 借用字段名，NULL 时返回 NULL。
 * @return 返回借用子节点，缺失为 NULL，使用期间保持父树有效并协调字段替换。
 */
cc_json_value_t *cc_json_object_get_case_sensitive(const cc_json_value_t *obj, const char *key);

/**
 * @brief 遍历对象子链表统计字段数。
 * @param[in] obj 借用的节点，可为 NULL。
 * @return 对象字段数，NULL 或其他类型为零。
 */
int cc_json_object_size(const cc_json_value_t *obj);

/**
 * @brief 按对象字段的链表位置读取名称。
 * @param[in] obj 借用的节点，须为对象才有结果。
 * @param[in] index 从零开始的字段下标，负数或越界返回 NULL。
 * @return 借用的字段名，缺失或非对象返回 NULL，父树销毁或该项替换后结束寿命。
 */
const char *cc_json_object_key_at(const cc_json_value_t *obj, int index);

/**
 * @brief 按对象字段的链表位置读取节点。
 * @param[in] obj 借用的节点，须为对象才有结果。
 * @param[in] index 从零开始的字段下标，负数或越界返回 NULL。
 * @return 借用的字段节点，缺失或非对象返回 NULL，父树销毁或该项替换后结束寿命。
 */
cc_json_value_t *cc_json_object_value_at(const cc_json_value_t *obj, int index);

/**
 * @brief 读取字符串节点的内部正文。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 借用的 NUL 结尾正文，NULL 或其他类型返回 NULL，节点销毁后结束寿命。
 */
const char *cc_json_string_value(const cc_json_value_t *value);

/**
 * @brief 读取数字节点的 int 缓存视图。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 数字的 int 视图，小数向零截断，超出 int 上下界时饱和，本接口构造的 NaN 为零，NULL 或其他类型返回零。
 */
int cc_json_int_value(const cc_json_value_t *value);

/**
 * @brief 读取数字节点的 double 视图。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 存储的 double，NULL 或其他类型返回 0.0，调用者负责所需范围、有限性及整数性校验。
 */
double cc_json_number_value(const cc_json_value_t *value);

/**
 * @brief 读取节点的布尔真值。
 * @param[in] value 借用的节点，可为 NULL。
 * @return JSON true 为一，JSON false、其他类型或 NULL 为零。
 */
int cc_json_bool_value(const cc_json_value_t *value);



/**
 * @brief 判断节点是否为对象。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 对象为一，其他类型或 NULL 为零。
 */
int cc_json_is_object(const cc_json_value_t *value);


/**
 * @brief 判断节点是否为数组。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 数组为一，其他类型或 NULL 为零。
 */
int cc_json_is_array(const cc_json_value_t *value);


/**
 * @brief 判断节点是否为字符串。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 字符串为一，其他类型或 NULL 为零。
 */
int cc_json_is_string(const cc_json_value_t *value);


/**
 * @brief 判断节点是否为数字。
 * @param[in] value 借用的节点，可为 NULL。
 * @return 数字为一，其他类型或 NULL 为零。
 */
int cc_json_is_number(const cc_json_value_t *value);


/**
 * @brief 判断节点是否为布尔值。
 * @param[in] value 借用的节点，可为 NULL。
 * @return JSON true 或 false 为一，其他类型或 NULL 为零。
 */
int cc_json_is_bool(const cc_json_value_t *value);


/**
 * @brief 判断节点是否为 JSON null。
 * @param[in] value 借用的节点，可为 NULL。
 * @return JSON null 节点为一，其他类型或 C 空指针为零。
 */
int cc_json_is_null(const cc_json_value_t *value);



/**
 * @brief 创建空对象节点。
 * @return 拥有的新节点，调用者销毁或成功挂接到父节点，分配失败返回 NULL。
 */
cc_json_value_t *cc_json_create_object(void);

/**
 * @brief 创建空数组节点。
 * @return 拥有的新节点，调用者销毁或成功挂接到父节点，分配失败返回 NULL。
 */
cc_json_value_t *cc_json_create_array(void);

/**
 * @brief 创建复制正文的字符串节点。
 * @param[in] value 借用的 NUL 结尾正文，NULL 采用空字符串。
 * @return 拥有的新节点，调用者销毁或成功交给父节点，分配失败返回 NULL。
 */
cc_json_value_t *cc_json_create_string(const char *value);

/**
 * @brief 创建 double 数字节点，同时生成安全的 int 缓存视图。
 * @param[in] value 待保存的 double，支持 NaN 和正负无穷大。NaN 的整数视图为零，无穷大按 int 上下界饱和。
 * @return 拥有的新节点，调用者销毁或成功交给父节点，分配失败返回 NULL。
 * @note 非有限值的 double 读取保留原值，序列化沿用 cJSON 的 JSON null 输出。
 */
cc_json_value_t *cc_json_create_number(double value);

/**
 * @brief 创建布尔节点。
 * @param[in] value 零生成 false，非零生成 true。
 * @return 拥有的新节点，调用者销毁或成功交给父节点，分配失败返回 NULL。
 */
cc_json_value_t *cc_json_create_bool(int value);

/**
 * @brief 创建 JSON null 节点。
 * @return 拥有的新节点，调用者销毁或成功交给父节点，分配失败返回 NULL。
 */
cc_json_value_t *cc_json_create_null(void);



/**
 * @brief 按区分大小写的 key 替换已有字段或追加新字段。
 * @pre obj 须为有效对象，value 须为独立拥有且尚未挂接的节点，与 obj 之间须无自引用或祖先关系。
 * @param[in,out] obj 接收字段的对象，NULL 时直接返回。
 * @param[in] key 调用期间借用的字段名，挂接时复制，NULL 时直接返回。
 * @param[in,out] value 待转移节点，NULL 时直接返回。挂接成功后由 obj 拥有，原同名节点随替换销毁。
 * @post 成功挂接时所有权已转移，NULL 参数或底层分配失败时仍由调用者拥有 value，函数以 void 返回，调用者需核对挂接结果。
 */
void cc_json_object_set(cc_json_value_t *obj, const char *key, cc_json_value_t *value);

/**
 * @brief 消费一个独占节点，挂接失败时自动销毁。
 * @pre 非空 obj 为对象，value 为独占且尚未挂接的节点，两者无自引用或祖先关系。
 * @param[in,out] obj 借用接收对象，NULL 时仍消费 value。
 * @param[in] key 调用期间借用的字段名，NULL 时仍消费 value。
 * @param[in,out] value 消费的节点，允许 NULL，成功归 obj，失败在本函数内销毁。
 * @return 成功为一，参数缺失或底层挂接失败为零，原节点拥有权均已交出。
 * @note 使用底层区分大小写的操作结果。key 可借用即将替换的旧项字段名，替换后结束该借用。
 */
int cc_json_object_set_owned(cc_json_value_t *obj, const char *key, cc_json_value_t *value);

/**
 * @brief 将独立节点追加到数组尾部。
 * @pre arr 须为数组，value 须为独立拥有且尚未挂接的节点，与 arr 之间须无自引用或祖先关系。
 * @param[in,out] arr 接收节点的数组，NULL 时直接返回。
 * @param[in,out] value 待转移节点，NULL 时直接返回。
 * @post 成功追加后 arr 拥有 value，空参数或底层拒绝时仍由调用者拥有，函数以 void 返回。
 */
void cc_json_array_append(cc_json_value_t *arr, cc_json_value_t *value);

/**
 * @brief 消费独占节点并追加到数组，失败时自动销毁该节点。
 * @pre value 为独占且尚未挂接的节点，与 arr 无自引用或祖先关系。
 * @param[in,out] arr 借用接收数组，NULL 或其他类型时仍消费 value。
 * @param[in,out] value 消费的节点，可为 NULL，成功由 arr 拥有，失败在此销毁。
 * @return 底层追加成功为一，参数或类型无效、底层拒绝时为零，原拥有权均已交出。
 */
int cc_json_array_append_owned(cc_json_value_t *arr, cc_json_value_t *value);

/**
 * @brief 读取数组的元素数量。
 * @param[in] arr 借用的节点，可为 NULL。
 * @return 数组元素数，NULL 或其他类型为零。
 */
int cc_json_array_size(const cc_json_value_t *arr);

/**
 * @brief 按下标从数组链表读取节点。
 * @param[in] arr 借用的节点，可为 NULL，须为数组才有结果。
 * @param[in] index 从零开始的下标，负数或越界返回 NULL。
 * @return 借用的元素节点，无有效元素返回 NULL，父树修改或销毁后需重新获取。
 */
cc_json_value_t *cc_json_array_get(const cc_json_value_t *arr, int index);

/**
 * @brief 取得数组首个元素，用于顺序遍历链表。
 * @param[in] arr 借用的节点，可为 NULL。
 * @return 借用的首元素，空数组、其他类型或 NULL 返回 NULL，遍历期间保持数组有效且稳定。
 */
const cc_json_value_t *cc_json_array_first(const cc_json_value_t *arr);
/**
 * @brief 取得当前数组元素的后继节点，完整遍历为线性开销。
 * @param[in] item first/next 返回的借用元素，可为 NULL，使用期间父数组保持有效且稳定。
 * @return 借用的后继节点，到达链表尾部或输入 NULL 时返回 NULL。
 */
const cc_json_value_t *cc_json_array_next(const cc_json_value_t *item);

#ifdef __cplusplus
}
#endif

#endif

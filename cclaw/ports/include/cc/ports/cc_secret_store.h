#ifndef CC_SECRET_STORE_H
#define CC_SECRET_STORE_H

#include "cc/core/cc_result.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CC_SECRET_STORE_VTABLE_VERSION 1U

enum {
    CC_SECRET_CAP_READ = 1ULL << 0,
    CC_SECRET_CAP_WRITE = 1ULL << 1,
    CC_SECRET_CAP_DELETE = 1ULL << 2,
    CC_SECRET_CAP_SECURE_AT_REST = 1ULL << 3,
};

typedef struct cc_secret_store_vtable cc_secret_store_vtable_t;

/* 拥有后端 self 的实例描述符，size/version 及 capabilities 由工厂填写，当前按完整布局访问。 */
typedef struct cc_secret_store {
    void *self;
    const cc_secret_store_vtable_t *vtable;
    size_t size;
    unsigned version;
    uint64_t capabilities;
} cc_secret_store_t;

struct cc_secret_store_vtable {
    size_t size;
    unsigned version;
    uint64_t capabilities;

    /**
     * @brief 同步按平台引用读取一个拥有的正文副本。
     * @param[in,out] self 借用存活后端状态，静态后端允许 NULL。
     * @param[in] reference 调用期间借用的 NUL 结尾引用，格式由平台解释。
     * @param[out] out_secret 必填空资源槽，成功取得附终止 NUL 的堆副本，调用者 cc_secret_free。
     * @return 拥有结果由调用者释放，失败没有新字符串交接。
     * @note 环境读取要求外层协调进程环境修改；实例寿命覆盖调用，具体后端操作同步返回。
     */
    cc_result_t (*get)(void *self, const char *reference, char **out_secret);
    /**
     * @brief 同步保存一个借用的 NUL 结尾正文。
     * @param[in,out] self 借用存活后端状态。
     * @param[in] reference 调用期间借用的平台引用。
     * @param[in] secret 调用期间借用的正文，后端保存成功后独立保管内容。
     * @return 拥有结果由调用者释放，只读或未配置后端可返回 UNSUPPORTED。
     * @note 持久写入失败的最终状态沿原生后端契约，调用者按需要重新读取核对。
     */
    cc_result_t (*set)(void *self, const char *reference, const char *secret);
    /**
     * @brief 同步删除平台引用对应的值。
     * @param[in,out] self 借用存活后端状态。
     * @param[in] reference 调用期间借用的 NUL 结尾引用。
     * @return 拥有结果由调用者释放，缺失项的幂等或 NOT_FOUND 语义由后端说明。
     * @note 实例寿命覆盖调用，同键并发更新按原生后端及使用方的业务同步处理。
     */
    cc_result_t (*remove)(void *self, const char *reference);
    /**
     * @brief 消费实例拥有的后端状态。
     * @param[in,out] self 被消费的状态，静态后端允许 NULL。
     * @pre 拥有者已关闭入口，并等待全部已接纳的读取、写入和删除返回。
     * @post 原 self 和其别名失效，拥有者结束对描述符的使用。
     */
    void (*destroy)(void *self);
};

/**
 * @brief 创建当前平台默认后端并填写版本与实例能力。
 * @param[out] out_store 必填空描述符，成功由调用者协调寿命并通过 vtable destroy 释放。
 * @return 成功为 CC_OK，参数、分配、初始化或平台未配置返回拥有结果，调用者释放。
 * @note POSIX 为 env 只读，Windows 使用 env/cred，ESP32 使用 NVS，通用 FreeRTOS 返回未配置。
 * 实例 capabilities 表示本次工厂确认的能力，vtable capabilities 为后端基础集合。
 */
cc_result_t cc_secret_store_get_default(cc_secret_store_t *out_store);

/**
 * @brief 通过 volatile 字节写清除指定区域。
 * @param[out] data 独占可写缓冲，NULL 时跳过。
 * @param[in] size 清除字节数，非空 data 须有足够容量。
 * @post 有效区域全部置零，缓冲仍归调用者。
 * @note 调用者排除区域的并发访问。
 */
void cc_secret_zero(void *data, size_t size);
/**
 * @brief 消费拥有的 NUL 结尾字符串，清除正文后释放。
 * @param[in,out] secret 兼容 malloc 的独占字符串，可为 NULL。
 * @post 首个 NUL 之前的字节清零，分配释放，旧地址失效。
 * @note 调用者先排空全部借用；按正文长度处理，分配容量由创建方管理。
 */
void cc_secret_free(char *secret);

#ifdef __cplusplus
}
#endif

#endif

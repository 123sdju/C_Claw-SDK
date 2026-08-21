

#ifndef CC_ARTIFACT_STORE_H
#define CC_ARTIFACT_STORE_H

#include "cc/core/cc_media.h"
#include "cc/core/cc_result.h"

/*
 * artifact store 接口对象前置声明。
 *
 * artifact store 独立于 message/session store，用于保存图片、音频、文件等较大资源。
 */
typedef struct cc_artifact_store cc_artifact_store_t;

/*
 * artifact store vtable。
 *
 * put/get/list/remove 都以 artifact 元数据为中心；实际二进制内容可以由 path/uri/
 * data_base64 表达。返回的 artifact/list 由调用方 cleanup。
 */
typedef struct cc_artifact_store_vtable {
    /* 保存 artifact；store 应深拷贝需要持久化的字段。 */
    cc_result_t (*put)(
        void *self,
        const char *session_id,
        const cc_media_artifact_t *artifact
    );
    /* 按 artifact_id 读取元数据；out_artifact 成功后由调用方 cleanup。 */
    cc_result_t (*get)(
        void *self,
        const char *artifact_id,
        cc_media_artifact_t *out_artifact
    );
    /* 列出 session 下的 artifacts；out_artifacts 成功后由调用方 cleanup。 */
    cc_result_t (*list)(
        void *self,
        const char *session_id,
        cc_media_artifact_list_t *out_artifacts
    );
    /* 删除 artifact 元数据或内容引用。 */
    cc_result_t (*remove)(void *self, const char *artifact_id);
    /* 销毁 adapter self。 */
    void (*destroy)(void *self);
} cc_artifact_store_vtable_t;

/* artifact store 接口对象：self + vtable 的 C OOP 组合。 */
struct cc_artifact_store {
    void *self;
    const cc_artifact_store_vtable_t *vtable;
};

#endif

#ifndef CC_JSON_FILE_SESSION_STORE_H
#define CC_JSON_FILE_SESSION_STORE_H

#include "cc/ports/cc_session_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建可读写的整文件 JSON 会话后端，缺失文件初始化为空会话库。
 * @param[in] file_path 文件路径借用，NULL 使用默认路径，后端复制路径。
 * @param[out] out_store 必需的新端口槽，先清零，成功后交付 self 与 vtable。
 * @return 成功或路径分配、默认文件系统、互斥量、文件初始化错误，调用者释放诊断。
 * @note 后端拥有文件系统端口、路径、JSON 树和互斥量，由 vtable.destroy 释放。
 *       父目录须预先存在。拥有者关闭调用入口并等待已接纳调用退出后再销毁。
 */
cc_result_t cc_json_file_store_create(const char *file_path, cc_session_store_t *out_store);

#ifdef __cplusplus
}
#endif

#endif

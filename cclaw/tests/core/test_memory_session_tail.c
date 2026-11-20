#include "cc/core/cc_message.h"
#include "cc/app/cc_session_manager.h"
#include "cc/ports/cc_session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 创建用于尾部加载和管理器借用检查的内存会话库。
 * @param[out] out_store 新端口槽，成功后由主函数 vtable.destroy 回收。
 * @return 构造状态与拥有诊断，调用者消费。
 */
extern cc_result_t cc_memory_session_store_create(cc_session_store_t *out_store);

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

/**
 * @brief 创建用户消息、同步追加并销毁输入消息。
 * @param[in,out] store 有效会话端口借用，由主函数拥有并串行使用。
 * @param[in] session 调用期非空会话 ID 借用，记录构造与端口分别复制。
 * @param[in] id 调用期消息 ID 兼正文借用。
 * @return 追加成功一，创建或追加失败零，消息与全部结果诊断在返回前清理。
 */
static int append(cc_session_store_t *store, const char *session, const char *id)
{
    cc_message_t *message = NULL;
    cc_result_t rc = cc_message_create_text(id, session, CC_ROLE_USER, id, NULL, &message);
    if (rc.code != CC_OK) { cc_result_free(&rc); return 0; }
    cc_result_free(&rc);
    cc_session_record_t record = { .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session, .data.message = message };
    rc = store->vtable->append_records(store->self, &record, 1);
    int ok = rc.code == CC_OK;
    cc_result_free(&rc);
    cc_message_destroy(message);
    return ok;
}

/**
 * @brief 检查会话管理器的列表输出参数、空列表兼容及底层存储借用责任。
 * @param[in,out] store 借用的有效内存存储，已持有两个会话的消息，本测试补建元数据，最终由主函数销毁。
 * @return 全部断言通过返回零，失败返回一。
 */
static int test_manager_list_outputs(cc_session_store_t *store)
{
    cc_session_manager_t *manager = NULL;
    cc_result_t rc = cc_session_manager_create(*store, &manager);
    CHECK(rc.code == CC_OK && manager);
    cc_result_free(&rc);
    rc = cc_session_manager_ensure_session(manager, "first", ".");
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    rc = cc_session_manager_ensure_session(manager, "other", ".");
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    cc_session_t *sessions = NULL;
    size_t count = 42;
    rc = cc_session_manager_list_sessions(manager, NULL, &count);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && count == 42);
    cc_result_free(&rc);
    rc = cc_session_manager_list_sessions(manager, &sessions, NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT && !sessions);
    cc_result_free(&rc);
    rc = cc_session_manager_list_sessions(NULL, NULL, NULL);
    CHECK(rc.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&rc);
    rc = cc_session_manager_list_sessions(NULL, &sessions, &count);
    CHECK(rc.code == CC_OK && !sessions && count == 0);
    cc_result_free(&rc);
    rc = cc_session_manager_list_sessions(manager, &sessions, &count);
    CHECK(rc.code == CC_OK && count == 2 && sessions);
    cc_result_free(&rc);
    cc_session_free_array(sessions, count);
    cc_session_manager_destroy(manager);
    sessions = NULL;
    count = 0;
    rc = store->vtable->list_sessions(store->self, &sessions, &count);
    CHECK(rc.code == CC_OK && count == 2 && sessions);
    cc_result_free(&rc);
    cc_session_free_array(sessions, count);
    return 0;
}

/**
 * @brief 验证内存历史保留最近消息，以及管理器列表参数和借用契约。
 * @return 全部断言通过返回零，失败返回一。
 */
int main(void)
{
    cc_session_store_t store = {0};
    cc_result_t rc = cc_memory_session_store_create(&store);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    CHECK(append(&store, "first", "a1"));
    CHECK(append(&store, "other", "b1"));
    CHECK(append(&store, "first", "a2"));
    CHECK(append(&store, "first", "a3"));

    cc_message_t *messages = NULL;
    size_t count = 0;
    rc = store.vtable->load_messages(store.self, "first", 2, &messages, &count);
    CHECK(rc.code == CC_OK);
    cc_result_free(&rc);
    CHECK(count == 2 && strcmp(messages[0].id, "a2") == 0 &&
          strcmp(messages[1].id, "a3") == 0);
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);

    messages = NULL;
    count = 0;
    rc = store.vtable->load_messages(store.self, "missing", 2, &messages, &count);
    CHECK(rc.code == CC_OK && count == 0 && messages == NULL);
    cc_result_free(&rc);
    CHECK(test_manager_list_outputs(&store) == 0);
    store.vtable->destroy(store.self);
    return 0;
}

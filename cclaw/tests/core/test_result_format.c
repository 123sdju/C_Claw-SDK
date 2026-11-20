#include "cc/core/cc_result.h"

#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CC_TEST_RESULT_FORMAT_FAILURES
static int s_format_failure;
static int s_format_calls;
static size_t s_large_allocation;

/**
 * @brief 声明链接包装所调用的真实格式化入口。
 * @param[out] buffer 可选输出缓冲，测量长度时为 NULL。
 * @param[in] capacity 缓冲容量，包含末尾 NUL。
 * @param[in] format 调用期间借用的格式串。
 * @param[in,out] args 交给标准库消费的参数列表。
 * @return 标准库所报告的输出长度，编码失败返回负值。
 */
int __real_vsnprintf(char *buffer, size_t capacity, const char *format, va_list args);

/**
 * @brief 声明链接包装所调用的真实分配入口。
 * @param[in] size 请求的字节数。
 * @return 返回调用者拥有的内存，分配失败返回 NULL。
 */
void *__real_malloc(size_t size);

/**
 * @brief 在格式化长度测量或输出阶段注入确定的失败。
 * @param[out] buffer 库传入的可选输出，测量阶段为 NULL。
 * @param[in] capacity 输出容量，测量阶段为零。
 * @param[in] format 调用期间借用的格式串。
 * @param[in,out] args 调用期间借用的参数列表，仅正常分支交给真实实现消费。
 * @return 按测试模式返回负长度、超容量长度或 INT_MAX，其余调用真实实现。
 */
int __wrap_vsnprintf(char *buffer, size_t capacity, const char *format, va_list args)
{
    if (!s_format_failure) return __real_vsnprintf(buffer, capacity, format, args);
    s_format_calls++;
    if (s_format_failure == 1) return -1;
    if (s_format_failure == 4) return INT_MAX;
    if (!buffer) return 1;
    if (s_format_failure == 2) return -1;
    if (capacity > 0) buffer[0] = '\0';
    return 3;
}

/**
 * @brief 捕获巨大格式长度对应的容量，避免测试真正分配数 GiB。
 * @param[in] size 请求的分配字节数。
 * @return 巨大长度测试中记录容量并返回 NULL，其他调用交给真实 malloc。
 */
void *__wrap_malloc(size_t size)
{
    if (s_format_failure == 4) {
        s_large_allocation = size;
        return NULL;
    }
    return __real_malloc(size);
}

/**
 * @brief 核对测量失败、写入失败、结果增长及 INT_MAX 长度的清理行为。
 * @post 各失败保留输入错误码并返回 NULL 消息，大长度按无符号容量请求。
 */
static void test_format_failures(void)
{
    for (int mode = 1; mode <= 4; mode++) {
        s_format_failure = mode;
        s_format_calls = 0;
        s_large_allocation = 0;
        cc_result_t result = cc_result_errf(CC_ERR_IO, "%s", "x");
        s_format_failure = 0;
        assert(result.code == CC_ERR_IO);
        assert(result.message == NULL);
        assert(s_format_calls == ((mode == 2 || mode == 3) ? 2 : 1));
        if (mode == 4) assert(s_large_allocation == (size_t)INT_MAX + 1U);
        cc_result_free(&result);
    }
}
#endif

/**
 * @brief 核对普通格式化、空描述及 NULL 格式串的公共行为。
 * @post 生成的消息独立拥有，释放后指针清空且错误码保留。
 */
static void test_normal_format(void)
{
    cc_result_t result = cc_result_errf(CC_ERR_TOOL, "%s %d", "tool", 7);
    assert(result.code == CC_ERR_TOOL);
    assert(result.message && strcmp(result.message, "tool 7") == 0);
    cc_result_free(&result);
    assert(result.message == NULL && result.code == CC_ERR_TOOL);
    cc_result_free(&result);

    result = cc_result_errf(CC_ERR_IO, "");
    assert(result.message && result.message[0] == '\0');
    cc_result_free(&result);
    result = cc_result_errf(CC_ERR_IO, NULL);
    assert(result.code == CC_ERR_IO && result.message == NULL);
    cc_result_free(&result);
}

/**
 * @brief 运行格式化结果的正常与故障回归。
 * @return 所有断言满足后返回零，断言失败由测试进程报告。
 */
int main(void)
{
    test_normal_format();
#if CC_TEST_RESULT_FORMAT_FAILURES
    test_format_failures();
#endif
    return 0;
}

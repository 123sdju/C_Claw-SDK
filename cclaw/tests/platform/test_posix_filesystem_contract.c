#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_path.h"
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* 私有包含实际生产代码，只在本翻译单元注入单次失败，SDK 分配和系统 I/O API 保持原样。 */
static int g_seek_fail;
static int g_tell_fail;
static int g_short_read;
static int g_close_fail;
static int g_malloc_fail;
static int g_read_interrupt;
static int g_write_interrupt;
static int g_sync_interrupt;
static int g_override_mtime;
static time_t g_mtime;
static const char *g_fake_length_string;

/**
 * @brief 对下一次生产定位注入错误。
 * @param[in,out] stream 借用的打开文件。
 * @param[in] offset 定位偏移字节数。
 * @param[in] origin 原生定位基准。
 * @return 注入时返回 -1，其余转发真实 fseek。
 */
static int model_fseek(FILE *stream, long offset, int origin)
{
    if (g_seek_fail) { g_seek_fail = 0; return -1; }
    return fseek(stream, offset, origin);
}

/**
 * @brief 注入负的文件长度，验证转换前校验。
 * @param[in,out] stream 借用的打开文件。
 * @return 注入时返回 -1，其余转发真实 ftell。
 */
static long model_ftell(FILE *stream)
{
    if (g_tell_fail) { g_tell_fail = 0; return -1; }
    return ftell(stream);
}

/**
 * @brief 模拟没有 ferror 的 EOF 短读。
 * @param[out] data 调用方的可写目标。
 * @param[in] size 元素字节数。
 * @param[in] count 元素数量。
 * @param[in,out] stream 借用打开文件。
 * @return 注入时少读一个元素，其余返回真实读取数量。
 */
static size_t model_fread(void *data, size_t size, size_t count, FILE *stream)
{
    if (g_short_read && count) { g_short_read = 0; count--; }
    return fread(data, size, count, stream);
}

/**
 * @brief 真实关闭文件后注入关闭错误，避免测试自身遗留句柄。
 * @param[in] stream 将结束拥有权的文件。
 * @return 注入时返回 EOF，其余返回真实 fclose 结果。
 */
static int model_fclose(FILE *stream)
{
    int result = fclose(stream);
    if (g_close_fail) { g_close_fail = 0; return EOF; }
    return result;
}

/**
 * @brief 对下一次生产 malloc 注入失败。
 * @param[in] size 分配字节数。
 * @return 注入时返回 NULL，其余返回调用方拥有的真实分配。
 */
static void *model_malloc(size_t size)
{
    if (g_malloc_fail) { g_malloc_fail = 0; return NULL; }
    return malloc(size);
}

/**
 * @brief 为指定借用字符串提供合成长度，覆盖无法真实分配的路径算术边界。
 * @param[in] text 借用的 NUL 结尾字符串。
 * @return 指定对象返回 SIZE_MAX-2，其余返回真实 strlen。
 */
static size_t model_strlen(const char *text)
{
    return text == g_fake_length_string ? SIZE_MAX - 2 : strlen(text);
}

/**
 * @brief 对根相对读取注入一次 EINTR。
 * @param[in] fd 借用文件描述符。
 * @param[out] data 可写 size 字节的缓冲。
 * @param[in] size 读取字节数。
 * @return 注入时返回 -1 并设置 EINTR，其余返回真实 read 结果。
 */
static ssize_t model_read(int fd, void *data, size_t size)
{
    if (g_read_interrupt) { g_read_interrupt = 0; errno = EINTR; return -1; }
    return read(fd, data, size);
}

/**
 * @brief 对根相对写入注入一次 EINTR。
 * @param[in] fd 借用文件描述符。
 * @param[in] data 借用 size 字节。
 * @param[in] size 写入字节数。
 * @return 注入时返回 -1 并设置 EINTR，其余返回真实 write 结果。
 */
static ssize_t model_write(int fd, const void *data, size_t size)
{
    if (g_write_interrupt) { g_write_interrupt = 0; errno = EINTR; return -1; }
    return write(fd, data, size);
}

/**
 * @brief 对生产同步注入一次 EINTR。
 * @param[in] fd 借用文件描述符。
 * @return 注入时返回 -1 并设置 EINTR，其余转发真实 fsync。
 */
static int model_fsync(int fd)
{
    if (g_sync_interrupt) { g_sync_interrupt = 0; errno = EINTR; return -1; }
    return fsync(fd);
}

/**
 * @brief 保留真实路径元数据，只替换待核验的修改时间。
 * @param[in] path 借用路径。
 * @param[out] value 成功时写入真实 stat 及可选合成 mtime。
 * @return 原样返回 lstat 结果。
 */
static int model_lstat(const char *path, struct stat *value)
{
    int result = lstat(path, value);
    if (result == 0 && g_override_mtime) value->st_mtime = g_mtime;
    return result;
}

/**
 * @brief 声明私有路径实现的父目录入口供前置 canonical 定义调用。
 * @param[in] path 借用路径。
 * @return 由下方实际生产定义交付拥有字符串或 NULL。
 */
char *model_path_dirname(const char *path);

#define fseek model_fseek
#define ftell model_ftell
#define fread model_fread
#define fclose model_fclose
#define malloc model_malloc
#define strlen model_strlen
#define read model_read
#define write model_write
#define fsync model_fsync
#define lstat model_lstat
#define cc_filesystem_get_posix model_filesystem_create
#define cc_filesystem_get_default model_filesystem_default
#define cc_path_join model_path_join
#define cc_path_canonical model_path_canonical
#define cc_path_is_within model_path_is_within
#define cc_path_dirname model_path_dirname
#define cc_path_exists model_path_exists
#include "../../platforms/posix/src/cc_posix_filesystem.c"
#include "../../platforms/posix/src/cc_posix_path.c"
#undef fseek
#undef ftell
#undef fread
#undef fclose
#undef malloc
#undef strlen
#undef read
#undef write
#undef fsync
#undef lstat
#undef cc_filesystem_get_posix
#undef cc_filesystem_get_default
#undef cc_path_join
#undef cc_path_canonical
#undef cc_path_is_within
#undef cc_path_dirname
#undef cc_path_exists

/**
 * @brief 核对结果码并归还拥有型动态成员。
 * @param[in] result 按值交接的结果，调用后不再持有其动态成员。
 * @param[in] expected 预期结果码。
 * @post 断言成立后结果动态成员已释放。
 */
static void expect_result(cc_result_t result, cc_error_code_t expected)
{
    assert(result.code == expected);
    cc_result_free(&result);
}

/**
 * @brief 通过真实文件验证二进制、尾读及失败时空输出。
 * @param[in,out] fs 借用存活端口。
 * @param[in] path 测试独占的文件路径。
 * @post 本次读取缓冲和结果释放，文件保留供后续场景使用。
 */
static void test_read_contract(cc_filesystem_t *fs, const char *path)
{
    const char bytes[] = {'a', '\0', 'b'};
    expect_result(fs->vtable->write_bytes(fs->self, path, bytes, sizeof(bytes)), CC_OK);
    void *data = NULL;
    size_t size = 0;
    expect_result(fs->vtable->read_bytes(fs->self, path, &data, &size), CC_OK);
    assert(size == sizeof(bytes) && !memcmp(data, bytes, size));
    free(data);
    char tail[2];
    expect_result(fs->vtable->read_tail(fs->self, path, tail, sizeof(tail), &size), CC_OK);
    assert(size == 2 && tail[0] == '\0' && tail[1] == 'b');
    char *text = (char *)(uintptr_t)1;
    g_seek_fail = 1;
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO);
    assert(!text);
    g_tell_fail = 1;
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO);
    assert(!text);
    g_short_read = 1;
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO);
    assert(!text);
    g_close_fail = 1;
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO);
    assert(!text);
    g_malloc_fail = 1;
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_ERR_OUT_OF_MEMORY);
    assert(!text);
    expect_result(fs->vtable->read_text(fs->self, path, &text), CC_OK);
    assert(!memcmp(text, bytes, sizeof(bytes)) && text[sizeof(bytes)] == '\0');
    free(text);
    text = (char *)(uintptr_t)1;
    expect_result(fs->vtable->read_text(fs->self, NULL, &text), CC_ERR_INVALID_ARGUMENT);
    assert(!text);
}

/**
 * @brief 验证根内 I/O 的 EINTR 重试、符号链接拒绝及 mkdir 类型冲突。
 * @param[in,out] fs 借用存活端口。
 * @param[in] root 测试独占目录。
 * @param[in] path root 下已存在的普通文件。
 * @post 本次读取分配、链接及路径副本清理，普通文件内容保留。
 */
static void test_relative_contract(cc_filesystem_t *fs, const char *root, const char *path)
{
    g_write_interrupt = g_sync_interrupt = 1;
    expect_result(fs->vtable->write_relative_text(fs->self, root, "data", "hello"), CC_OK);
    assert(!g_write_interrupt && !g_sync_interrupt);
    char *text = NULL;
    g_read_interrupt = 1;
    expect_result(fs->vtable->read_relative_text(fs->self, root, "data", &text), CC_OK);
    assert(!g_read_interrupt && !strcmp(text, "hello"));
    free(text);
    const char binary[] = {'x', '\0', 'y'};
    g_write_interrupt = g_sync_interrupt = 1;
    expect_result(fs->vtable->write_relative_bytes(fs->self, root, "data", binary, sizeof(binary)), CC_OK);
    assert(!g_write_interrupt && !g_sync_interrupt);
    expect_result(fs->vtable->make_dir(fs->self, path), CC_ERR_IO);
    char *link_path = model_path_join(root, "link");
    assert(link_path && symlink(root, link_path) == 0);
    expect_result(fs->vtable->write_relative_text(fs->self, root, "link/created", "bad"), CC_ERR_PERMISSION_DENIED);
    char *forbidden = model_path_join(root, "created");
    assert(forbidden && access(forbidden, F_OK) != 0);
    expect_result(fs->vtable->read_relative_text(fs->self, root, "link/data", &text), CC_ERR_PERMISSION_DENIED);
    assert(!text);
    assert(unlink(link_path) == 0);
    free(forbidden);
    free(link_path);
    g_sync_interrupt = 1;
    expect_result(fs->vtable->sync_file(fs->self, path), CC_OK);
    g_sync_interrupt = 1;
    expect_result(fs->vtable->sync_dir(fs->self, root), CC_OK);
}

/**
 * @brief 验证 Unix mtime 的负值、普通值和毫秒饱和，以及路径长度计算和词法结果。
 * @param[in,out] fs 借用存活端口。
 * @param[in] root 测试独占目录。
 * @param[in] path 测试已存在的普通文件路径。
 * @post 合成元数据及长度注入关闭，所有路径副本已释放。
 */
static void test_metadata_and_paths(cc_filesystem_t *fs, const char *root, const char *path)
{
    cc_filesystem_stat_t st;
    g_override_mtime = 1;
    g_mtime = (time_t)-1;
    expect_result(fs->vtable->stat(fs->self, path, &st), CC_OK);
    assert(st.modified_ms == 0);
    g_mtime = 42;
    expect_result(fs->vtable->stat(fs->self, path, &st), CC_OK);
    assert(st.modified_ms == 42000);
    if (sizeof(time_t) >= sizeof(int64_t)) {
        g_mtime = (time_t)INT64_MAX;
        expect_result(fs->vtable->stat(fs->self, path, &st), CC_OK);
        assert(st.modified_ms == UINT64_MAX);
    }
    g_override_mtime = 0;
    char base[] = "a";
    g_fake_length_string = base;
    assert(!model_path_join(base, "bc"));
    assert(!model_path_join(base, "bcd"));
    g_fake_length_string = NULL;
    char *joined = model_path_join(root, "/absolute");
    assert(joined && !strcmp(joined, "/absolute"));
    free(joined);
    joined = model_path_dirname(path);
    assert(joined && !strcmp(joined, root));
    free(joined);
    assert(model_path_is_within(root, path));
    joined = model_path_join(root, "../outside");
    assert(joined && !model_path_is_within(root, joined));
    free(joined);
}

/**
 * @brief 在 Linux 原生文件系统上运行私有生产实现及单次故障注入回归。
 * @return 全部断言成立返回 0，失败由 assert 终止。
 * @note 模型不覆盖断电、32 位 off_t/size_t 组合或不同 POSIX 内核的 close/EINTR 行为。
 */
int main(void)
{
    char directory[] = "/tmp/cclaw_fs_contract_XXXXXX";
    assert(mkdtemp(directory));
    cc_filesystem_t fs = {0};
    expect_result(model_filesystem_create(&fs), CC_OK);
    char *path = model_path_join(directory, "data");
    assert(path);
    test_read_contract(&fs, path);
    test_relative_contract(&fs, directory, path);
    test_metadata_and_paths(&fs, directory, path);
    expect_result(fs.vtable->remove(fs.self, path), CC_OK);
    free(path);
    fs.vtable->destroy(fs.self);
    assert(rmdir(directory) == 0);
    return 0;
}

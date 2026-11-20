#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "cc/ports/cc_filesystem.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief 失败时报告行号并返回一。 @param[in] expression 求值一次的条件。 */
#define CHECK(expression) do { if (!(expression)) { \
    fprintf(stderr, "ESP filesystem contract failed at %d\n", __LINE__); return 1; \
} } while (0)

enum failure_kind { NONE, READ, WRITE, FLUSH, SYNC, CLOSE, STREAM, TEMPORARY, DIRECTORY, DIRECTORY_CLOSE };
static enum failure_kind next_failure;
static size_t allocation_count;
static size_t failed_allocation = SIZE_MAX;
static int emulate_fat_rename;
static unsigned rename_calls;
static unsigned failed_renames;

/** @brief 消费一次指定 I/O 故障。 @param[in] kind 当前调用类型。 @return 命中故障为一。 */
static int should_fail(enum failure_kind kind)
{
    if (next_failure != kind) return 0;
    next_failure = NONE;
    errno = EIO;
    return 1;
}

/** @brief 注入端口局部分配失败。 @param[in] size 字节数。 @return 堆地址或 NULL。 */
static void *fixture_malloc(size_t size)
{
    return allocation_count++ == failed_allocation ? NULL : malloc(size);
}

/** @brief 注入端口零分配失败。 @param[in] count 元素数。 @param[in] size 单项字节。 @return 堆地址或 NULL。 */
static void *fixture_calloc(size_t count, size_t size)
{
    return allocation_count++ == failed_allocation ? NULL : calloc(count, size);
}

/** @brief 注入端口扩容失败。 @param[in,out] data 旧分配。 @param[in] size 新容量。 @return 新地址或 NULL，失败保留旧块。 */
static void *fixture_realloc(void *data, size_t size)
{
    return allocation_count++ == failed_allocation ? NULL : realloc(data, size);
}

/** @brief 模拟无 ferror 的短读。 @param[out] data 缓冲。 @param[in] size 元素字节。 @param[in] count 元素数。 @param[in,out] file 打开流。 @return 实际元素数。 */
static size_t fixture_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (count && should_fail(READ)) count--;
    return fread(data, size, count, file);
}

/** @brief 模拟无 ferror 的短写。 @param[in] data 正文。 @param[in] size 元素字节。 @param[in] count 元素数。 @param[in,out] file 打开流。 @return 实际元素数。 */
static size_t fixture_fwrite(const void *data, size_t size, size_t count, FILE *file)
{
    if (count && should_fail(WRITE)) count--;
    return fwrite(data, size, count, file);
}

/** @brief 刷新真实流后注入错误报告。 @param[in,out] file 打开流。 @return 原生值或 EOF。 */
static int fixture_fflush(FILE *file)
{
    int rc = fflush(file);
    return should_fail(FLUSH) ? EOF : rc;
}

/** @brief 同步真实描述符或注入失败。 @param[in] descriptor 打开描述符。 @return 原生值或负一。 */
static int fixture_fsync(int descriptor)
{
    return should_fail(SYNC) ? -1 : fsync(descriptor);
}

/** @brief 关闭真实流后注入错误报告。 @param[in,out] file 被消费的流。 @return 原生值或 EOF。 */
static int fixture_fclose(FILE *file)
{
    int rc = fclose(file);
    return should_fail(CLOSE) ? EOF : rc;
}

/** @brief 创建实际独占临时文件或注入失败。 @param[in,out] path 可写模板。 @return 描述符或负一。 */
static int fixture_mkstemp(char *path)
{
    return should_fail(TEMPORARY) ? -1 : mkstemp(path);
}

/** @brief 创建文件流或注入失败。 @param[in] descriptor 打开描述符。 @param[in] mode 借用模式。 @return 流或 NULL，失败由端口关闭描述符。 */
static FILE *fixture_fdopen(int descriptor, const char *mode)
{
    return should_fail(STREAM) ? NULL : fdopen(descriptor, mode);
}

/** @brief 注入目录读取失败。 @param[in,out] directory 打开目录。 @return 借用项或 NULL。 */
static struct dirent *fixture_readdir(DIR *directory)
{
    return should_fail(DIRECTORY) ? NULL : readdir(directory);
}

/** @brief 关闭真实目录后注入错误报告。 @param[in,out] directory 被消费目录。 @return 原生值或负一。 */
static int fixture_closedir(DIR *directory)
{
    int rc = closedir(directory);
    return should_fail(DIRECTORY_CLOSE) ? -1 : rc;
}

/** @brief 模拟 FAT 覆盖限制与指定阶段故障。 @param[in] source 借用源路径。 @param[in] destination 借用目标。 @return rename 返回值。 */
static int fixture_rename(const char *source, const char *destination)
{
    rename_calls++;
    if (rename_calls < 32U && (failed_renames & (1U << rename_calls))) { errno = EIO; return -1; }
    if (emulate_fat_rename && access(destination, F_OK) == 0) { errno = EEXIST; return -1; }
    return rename(source, destination);
}

/** @brief 接收日志实参且保持夹具无日志分配。 @param[in] tag 借用标签。 @param[in] format 借用格式。 @param[in] ... 按值传入的格式参数。 @post 日志参数消费完毕，无外部状态变化。 */
static void fixture_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_PLATFORM 1
#ifdef CC_TEST_ESP_SFN
#define CONFIG_FATFS_LFN_NONE 1
#endif
#define cc_filesystem_get_default fixture_esp_default
#define cc_filesystem_get_posix fixture_esp_alias
#define malloc fixture_malloc
#define calloc fixture_calloc
#define realloc fixture_realloc
#define fread fixture_fread
#define fwrite fixture_fwrite
#define fflush fixture_fflush
#define fsync fixture_fsync
#define fclose fixture_fclose
#define mkstemp fixture_mkstemp
#define fdopen fixture_fdopen
#define readdir fixture_readdir
#define closedir fixture_closedir
#define rename fixture_rename
#ifndef CC_TEST_ESP_FILESYSTEM_SOURCE
#define CC_TEST_ESP_FILESYSTEM_SOURCE "../../platforms/esp32/src/cc_esp32_filesystem.c"
#endif
#include CC_TEST_ESP_FILESYSTEM_SOURCE
#undef rename
#undef closedir
#undef readdir
#undef fdopen
#undef mkstemp
#undef fclose
#undef fsync
#undef fflush
#undef fwrite
#undef fread
#undef realloc
#undef calloc
#undef malloc
#undef cc_filesystem_get_posix
#undef cc_filesystem_get_default

/** @brief 消费结果并判断错误码。 @param[in] rc 接管结果。 @param[in] code 期望码。 @return 匹配为一。 */
static int result_is(cc_result_t rc, cc_error_code_t code)
{
    int matched = rc.code == code;
    cc_result_free(&rc);
    return matched;
}

/** @brief 组合夹具路径。 @param[in] root 借用根。 @param[in] name 借用名称。 @param[out] out 至少 512 字节缓冲。 @return 完整写入为一。 */
static int path_of(const char *root, const char *name, char *out)
{
    int written = snprintf(out, 512, "%s/%s", root, name);
    return written >= 0 && written < 512;
}

/** @brief 通过真实 stdio 设置夹具文件。 @param[in] path 路径。 @param[in] data 正文。 @param[in] size 字节数。 @return 完整写入并关闭成功为一。 */
static int put_bytes(const char *path, const void *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int good = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) good = 0;
    return good;
}

/** @brief 核对小文件完整内容。 @param[in] path 路径。 @param[in] expected 期望正文。 @return 内容及长度相同为一。 */
static int text_is(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    char data[128];
    size_t size = fread(data, 1, sizeof(data), file);
    int good = !ferror(file) && size == strlen(expected) && memcmp(data, expected, size) == 0;
    if (fclose(file) != 0) good = 0;
    return good;
}

/** @brief 释放独占目录名称数组。 @param[in,out] items 拥有数组。 @param[in] count 有效项数。 @post 所有名称及数组归还。 */
static void free_names(char **items, size_t count)
{
    for (size_t i = 0; i < count; i++) free(items[i]);
    free(items);
}

/**
 * @brief 检查根路径、完整读取、尾部、目录及失败输出约定。
 * @param[in,out] fs 存活夹具端口借用。
 * @param[in] root 测试拥有的临时根目录。
 * @return 全部通过为零，失败为一。
 */
static int test_io_contracts(cc_filesystem_t *fs, const char *root)
{
    char path[512];
    CHECK(path_of(root, "entry", path));
    CHECK(put_bytes(path, "data", 4));
    CHECK(result_is(fs->vtable->exists(fs->self, path, NULL), CC_ERR_INVALID_ARGUMENT));
    int exists = 2;
    CHECK(result_is(fs->vtable->exists(fs->self, NULL, &exists), CC_ERR_INVALID_ARGUMENT) && exists == 0);
    char *text = (char *)(uintptr_t)1;
    CHECK(result_is(fs->vtable->read_relative_text(fs->self, "", "entry", &text), CC_ERR_PERMISSION_DENIED) && !text);
    CHECK(result_is(fs->vtable->write_relative_bytes(fs->self, root, "entry\001ignored", "bad", 3), CC_ERR_PERMISSION_DENIED));
    char bad_root[512];
    CHECK(snprintf(bad_root, sizeof(bad_root), "%s%cignored", root, 1) > 0);
    CHECK(result_is(fs->vtable->write_relative_bytes(fs->self, bad_root, "entry", "bad", 3), CC_ERR_PERMISSION_DENIED));
#ifdef CC_TEST_ESP_SFN
    CHECK(snprintf(bad_root, sizeof(bad_root), "%s ignored", root) > 0);
    CHECK(result_is(fs->vtable->write_relative_bytes(fs->self, bad_root, "entry", "bad", 3), CC_ERR_PERMISSION_DENIED));
#endif
    CHECK(text_is(path, "data"));
    CHECK(result_is(fs->vtable->read_text(fs->self, path, &text), CC_OK) && strcmp(text, "data") == 0);
    free(text);
    next_failure = READ;
    CHECK(result_is(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO) && !text);
    next_failure = CLOSE;
    CHECK(result_is(fs->vtable->read_text(fs->self, path, &text), CC_ERR_IO) && !text);
    CHECK(truncate(path, 1024 * 1024 + 1) == 0);
    CHECK(result_is(fs->vtable->read_text(fs->self, path, &text), CC_ERR_LIMIT_EXCEEDED) && !text);
    const char binary[] = {'a', 0, 'b'};
    CHECK(put_bytes(path, binary, sizeof(binary)));
    CHECK(result_is(fs->vtable->read_text(fs->self, path, &text), CC_ERR_INVALID_ARGUMENT) && !text);
    void *data = NULL;
    size_t size = 9;
    CHECK(result_is(fs->vtable->read_bytes(fs->self, path, &data, &size), CC_OK));
    CHECK(size == sizeof(binary) && memcmp(data, binary, size) == 0);
    free(data);
    char tail[2];
    CHECK(result_is(fs->vtable->read_tail(fs->self, path, tail, sizeof(tail), &size), CC_OK));
    CHECK(size == 2 && tail[0] == 0 && tail[1] == 'b');
    next_failure = READ;
    CHECK(result_is(fs->vtable->read_tail(fs->self, path, tail, sizeof(tail), &size), CC_ERR_IO) && !size);
    CHECK(result_is(fs->vtable->make_dir(fs->self, path), CC_ERR_IO));
    char **items = (char **)(uintptr_t)1;
    size_t count = 3;
    CHECK(result_is(fs->vtable->list_dir(fs->self, root, &items, NULL), CC_ERR_INVALID_ARGUMENT) && !items);
    next_failure = DIRECTORY;
    CHECK(result_is(fs->vtable->list_dir(fs->self, root, &items, &count), CC_ERR_IO) && !items && !count);
    next_failure = DIRECTORY_CLOSE;
    CHECK(result_is(fs->vtable->list_dir(fs->self, root, &items, &count), CC_ERR_IO) && !items && !count);
    CHECK(result_is(fs->vtable->list_dir(fs->self, root, &items, &count), CC_OK) && count > 0);
    free_names(items, count);
#ifdef CC_TEST_ESP_SFN
    const cc_error_code_t space_result = CC_ERR_PERMISSION_DENIED;
#else
    const cc_error_code_t space_result = CC_OK;
#endif
    CHECK(result_is(fs->vtable->write_relative_bytes(fs->self, root, "with space", "x", 1), space_result));
    return 0;
}

/**
 * @brief 检查独占临时文件、各提交前失败与逐分配撤销。
 * @param[in,out] fs 存活端口借用。
 * @param[in] root 测试拥有的临时根目录。
 * @return 全部通过为零，失败为一，旧用户 .tmp 文件在各场景保留。
 */
static int test_temporary_install(cc_filesystem_t *fs, const char *root)
{
    char path[512], old_temporary[512];
    CHECK(path_of(root, "entry", path) && path_of(root, "entry.tmp", old_temporary));
    CHECK(put_bytes(old_temporary, "keep", 4));
    CHECK(result_is(fs->vtable->write_relative_text(fs->self, root, "entry", "new"), CC_OK));
    CHECK(text_is(path, "new") && text_is(old_temporary, "keep"));
    const enum failure_kind failures[] = {WRITE, FLUSH, SYNC, CLOSE, STREAM, TEMPORARY};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        CHECK(put_bytes(path, "old", 3));
        next_failure = failures[i];
        CHECK(result_is(fs->vtable->write_relative_text(fs->self, root, "entry", "new"), CC_ERR_IO));
        CHECK(text_is(path, "old") && text_is(old_temporary, "keep"));
    }
    allocation_count = 0;
    CHECK(result_is(fs->vtable->write_relative_text(fs->self, root, "entry", "new"), CC_OK));
    size_t allocations = allocation_count;
    CHECK(allocations > 0);
    for (size_t failure = 0; failure < allocations; failure++) {
        CHECK(put_bytes(path, "old", 3));
        allocation_count = 0;
        failed_allocation = failure;
        cc_result_t rc = fs->vtable->write_relative_text(fs->self, root, "entry", "new");
        failed_allocation = SIZE_MAX;
        CHECK(result_is(rc, CC_ERR_OUT_OF_MEMORY));
        CHECK(text_is(path, "old") && text_is(old_temporary, "keep"));
    }
    return 0;
}

/**
 * @brief 模拟 FAT 两步安装、恢复失败后的候选及备用源碰撞。
 * @param[in,out] fs 存活端口借用。
 * @param[in] root 测试拥有的临时根目录。
 * @return 全部断言通过为零，失败为一。
 */
static int test_replacement_recovery(cc_filesystem_t *fs, const char *root)
{
    char path[512], source[512], backup[512];
    CHECK(path_of(root, "entry", path) && path_of(root, "candidate", source) && path_of(root, "entry.bak", backup));
    CHECK(put_bytes(path, "old", 3) && put_bytes(source, "new", 3));
    emulate_fat_rename = 1;
    rename_calls = 0;
    failed_renames = 1U << 3;
    CHECK(result_is(fs->vtable->atomic_replace(fs->self, source, path), CC_ERR_IO));
    CHECK(text_is(path, "old") && text_is(source, "new") && access(backup, F_OK) != 0);
    rename_calls = 0;
    failed_renames = (1U << 3) | (1U << 4);
    CHECK(result_is(fs->vtable->atomic_replace(fs->self, source, path), CC_ERR_IO));
    CHECK(access(path, F_OK) != 0 && text_is(backup, "old") && text_is(source, "new"));
    rename_calls = 0;
    failed_renames = 0;
    CHECK(result_is(fs->vtable->atomic_replace(fs->self, source, path), CC_OK));
    CHECK(text_is(path, "new") && access(source, F_OK) != 0 && access(backup, F_OK) != 0);
    CHECK(put_bytes(backup, "saved", 5));
    CHECK(result_is(fs->vtable->atomic_replace(fs->self, backup, path), CC_ERR_INVALID_ARGUMENT));
    CHECK(text_is(path, "new") && text_is(backup, "saved"));
    emulate_fat_rename = 0;
    return 0;
}

/** @brief 递归清理本测试创建的目录树。 @param[in] path 独占夹具根或子路径。 @return 完整清理为零，失败为一。 */
static int remove_fixture(const char *path)
{
    struct stat info;
    CHECK(lstat(path, &info) == 0);
    if (!S_ISDIR(info.st_mode)) return unlink(path) == 0 ? 0 : 1;
    DIR *directory = opendir(path);
    CHECK(directory);
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[512];
        CHECK(path_of(path, entry->d_name, child));
        CHECK(remove_fixture(child) == 0);
    }
    CHECK(closedir(directory) == 0);
    return rmdir(path) == 0 ? 0 : 1;
}

/**
 * @brief 以真实文件及可控原生错误验证实际 ESP32 端口，可单独运行临时文件负对照。
 * @param[in] argc 命令行参数数量。
 * @param[in] argv 借用参数数组，首参数 temp 仅选择临时安装场景。
 * @return 全部通过为零，失败为一，成功路径销毁端口并清理自建目录。
 */
int main(int argc, char **argv)
{
    char root[] = "/tmp/cclaw-esp-fs-XXXXXX";
    CHECK(mkdtemp(root));
    cc_filesystem_t fs = {0};
    CHECK(result_is(fixture_esp_default(&fs), CC_OK));
    if (argc > 1 && strcmp(argv[1], "temp") == 0) {
        CHECK(test_temporary_install(&fs, root) == 0);
    } else {
        CHECK(test_io_contracts(&fs, root) == 0);
        CHECK(test_temporary_install(&fs, root) == 0);
        CHECK(test_replacement_recovery(&fs, root) == 0);
    }
    fs.vtable->destroy(fs.self);
    CHECK(remove_fixture(root) == 0);
    return 0;
}

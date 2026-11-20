#include "cc/ports/cc_filesystem.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef CCLAW_FREERTOS_ENABLE_FATFS
#define CCLAW_FREERTOS_ENABLE_FATFS 0
#endif

/* 私有包含实际适配器，只替换本翻译单元分配入口，SDK 公共分配接口保持原样。 */
static void *g_owned[128];
static size_t g_owned_count;
static size_t g_alloc_calls;
static size_t g_fail_at;

/**
 * @brief 分配并登记测试所包含生产代码的缓冲。
 * @param[in] size 所需字节数。
 * @return 指定失败点返回 NULL，其余返回调用方拥有的分配。
 */
static void *model_malloc(size_t size)
{
    if (++g_alloc_calls == g_fail_at) return NULL;
    void *value = malloc(size);
    if (value) {
        assert(g_owned_count < sizeof(g_owned) / sizeof(g_owned[0]));
        g_owned[g_owned_count++] = value;
    }
    return value;
}

/**
 * @brief 清零分配端口对象并支持同一故障序列。
 * @param[in] count 元素数。
 * @param[in] size 每元素字节数。
 * @return 成功返回拥有型清零缓冲，乘法溢出或指定失败返回 NULL。
 */
static void *model_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *value = model_malloc(count * size);
    if (value) memset(value, 0, count * size);
    return value;
}

/**
 * @brief 释放登记的生产分配并核对唯一拥有权。
 * @param[in] value 交接独占分配，NULL 安全。
 * @post 对应分配从登记表删除并释放。
 */
static void model_free(void *value)
{
    if (!value) return;
    size_t index = 0;
    while (index < g_owned_count && g_owned[index] != value) index++;
    assert(index < g_owned_count);
    g_owned[index] = g_owned[--g_owned_count];
    free(value);
}

/**
 * @brief 为目录数组提供保留旧分配的可失败扩容。
 * @param[in] old 当前独占分配，NULL 表示首次分配。
 * @param[in] size 所需字节数，测试调用均非零。
 * @return 成功返回新拥有型指针，失败保留 old 及其登记记录。
 */
static void *model_realloc(void *old, size_t size)
{
    if (!old) return model_malloc(size);
    if (++g_alloc_calls == g_fail_at) return NULL;
    size_t index = 0;
    while (index < g_owned_count && g_owned[index] != old) index++;
    assert(index < g_owned_count);
    void *value = realloc(old, size);
    if (value) g_owned[index] = value;
    return value;
}

#define malloc model_malloc
#define calloc model_calloc
#define realloc model_realloc
#define free model_free
#define cc_filesystem_get_default model_get_default
#define cc_filesystem_get_posix model_get_posix
#include "../../platforms/freertos/src/cc_freertos_filesystem.c"
#undef cc_filesystem_get_default
#undef cc_filesystem_get_posix
#undef malloc
#undef calloc
#undef realloc
#undef free

/**
 * @brief 核对并释放一个拥有型 SDK 结果。
 * @param[in] result 按值交接待验证结果。
 * @param[in] code 预期错误码。
 * @post 结果匹配且动态诊断被释放。
 */
static void expect_result(cc_result_t result, cc_error_code_t code)
{
    assert(result.code == code);
    cc_result_free(&result);
}

#if CCLAW_FREERTOS_ENABLE_FATFS

/* 原生替身保留 close 失败后的 share 状态，不将失败伪装成已释放。 */
static FSIZE_t g_length;
static char g_last_path[256];
static unsigned g_open_calls;
static unsigned g_close_calls;
static unsigned g_read_calls;
static unsigned g_mutations;
static unsigned g_unlink_calls;
static unsigned g_sync_dirty;
static unsigned g_dir_closed;
static int g_share_held;
static int g_close_failure;
static int g_short_read;
static int g_short_write;
static int g_read_failure;
static int g_directory_failure;
static int g_closedir_failure;
static int g_parent_missing;
static int g_existing_file;

/**
 * @brief 打开模型文件并记录适配器传入的原生路径。
 * @param[out] file 必需对象，成功初始化长度、偏移和打开状态。
 * @param[in] path 借用 NUL 结尾路径。
 * @param[in] mode FatFs 打开标志，覆盖模式记录文件副作用。
 * @return 模拟未建父目录、遗留 share 冲突或成功。
 */
FRESULT f_open(FIL *file, const TCHAR *path, unsigned char mode)
{
    g_open_calls++;
    assert(strlen(path) < sizeof(g_last_path));
    strcpy(g_last_path, path);
#if FF_FS_LOCK
    if (g_share_held) return FR_LOCKED;
#endif
    if (g_parent_missing) return FR_NO_PATH;
    memset(file, 0, sizeof(*file));
    file->open = 1;
    file->length = g_length;
    if (mode & FA_CREATE_ALWAYS) {
        file->length = 0;
        g_mutations++;
    }
    if ((mode & FA_OPEN_APPEND) == FA_OPEN_APPEND) file->position = file->length;
    return FR_OK;
}

/**
 * @brief 用确定性字节填充生产代码的读取缓冲。
 * @param[in,out] file 必需打开对象，成功推进偏移。
 * @param[out] data 至少 size 字节的输出区。
 * @param[in] size 本次 UINT 字节数。
 * @param[out] got 实际长度，故障时为零，短读时少一字节。
 * @return 指定故障返回 DISK_ERR，其余返回 FR_OK。
 */
FRESULT f_read(FIL *file, void *data, UINT size, UINT *got)
{
    assert(file->open);
    g_read_calls++;
    *got = 0;
    if (g_read_failure) return FR_DISK_ERR;
    *got = g_short_read && size ? (UINT)(size - 1) : size;
    for (UINT index = 0; index < *got; index++) {
        ((unsigned char *)data)[index] = (unsigned char)('a' + (file->position + index) % 26);
    }
    file->position += *got;
    return FR_OK;
}

/**
 * @brief 记录写入副作用和脏状态，支持成功短写。
 * @param[in,out] file 必需打开对象，推进偏移并标脏。
 * @param[in] data 借用 size 字节，适配器零长度传空串。
 * @param[in] size 本次字节数。
 * @param[out] wrote 实际字节数。
 * @return 始终 FR_OK，短写通过 wrote 表示。
 */
FRESULT f_write(FIL *file, const void *data, UINT size, UINT *wrote)
{
    assert(file->open && data);
    *wrote = g_short_write && size ? (UINT)(size - 1) : size;
    file->position += *wrote;
    if (file->position > file->length) file->length = file->position;
    file->modified = 1;
    g_mutations++;
    return FR_OK;
}

/**
 * @brief 设置尾部读取使用的模型文件偏移。
 * @param[in,out] file 必需打开对象。
 * @param[in] offset 字节偏移，不得超过打开时长度。
 * @return 有效偏移返回 FR_OK。
 */
FRESULT f_lseek(FIL *file, FSIZE_t offset)
{
    assert(file->open && offset <= file->length);
    file->position = offset;
    return FR_OK;
}

/**
 * @brief 模拟仅脏 FIL 会向驱动提交同步的原生规则。
 * @param[in,out] file 必需打开对象，成功清除脏标记。
 * @return 关闭故障开关启用时返回 DISK_ERR，否则 FR_OK。
 */
FRESULT f_sync(FIL *file)
{
    assert(file->open);
    if (g_close_failure) return FR_DISK_ERR;
    if (file->modified) g_sync_dirty++;
    file->modified = 0;
    return FR_OK;
}

/**
 * @brief 模拟 f_close 的先同步、成功才使对象和 share 失效顺序。
 * @param[in,out] file 必需打开对象，失败保持打开状态。
 * @return 同步故障原样返回，成功返回 FR_OK。
 */
FRESULT f_close(FIL *file)
{
    g_close_calls++;
    FRESULT result = f_sync(file);
    if (result != FR_OK) {
#if FF_FS_LOCK
        g_share_held = 1;
#endif
        return result;
    }
    file->open = 0;
    return FR_OK;
}

/**
 * @brief 生成目录冲突和普通文件元数据。
 * @param[in] path 借用路径，含 missing 时模拟不存在。
 * @param[out] info 必需元数据槽。
 * @return 缺失返回 NO_FILE，其余返回 FR_OK。
 */
FRESULT f_stat(const TCHAR *path, FILINFO *info)
{
    memset(info, 0, sizeof(*info));
    if (strstr(path, "missing")) return FR_NO_FILE;
    info->fattrib = g_existing_file ? 0 : AM_DIR;
    info->fsize = g_length;
    return FR_OK;
}

/**
 * @brief 模拟目录创建和已有文件冲突。
 * @param[in] path 借用原生路径，仅核对有效性。
 * @return 冲突模式返回 EXIST，否则记录创建并返回 FR_OK。
 */
FRESULT f_mkdir(const TCHAR *path)
{
    assert(path && path[0]);
    if (g_existing_file) return FR_EXIST;
    g_mutations++;
    g_parent_missing = 0;
    return FR_OK;
}

/**
 * @brief 记录删除调用，以证明不支持的原子替换没有副作用。
 * @param[in] path 借用原生路径。
 * @return 始终返回 FR_OK。
 */
FRESULT f_unlink(const TCHAR *path)
{
    assert(path);
    g_unlink_calls++;
    g_mutations++;
    return FR_OK;
}

/**
 * @brief 初始化两项枚举的模型目录。
 * @param[out] dir 必需目录对象。
 * @param[in] path 借用原生路径。
 * @return 始终返回 FR_OK。
 */
FRESULT f_opendir(DIR *dir, const TCHAR *path)
{
    assert(path);
    dir->index = 0;
    dir->open = 1;
    return FR_OK;
}

/**
 * @brief 逐项返回两个名称，支持第二项原生读取失败。
 * @param[in,out] dir 必需打开目录，成功推进游标。
 * @param[out] info 输出名称，结束时首字节为 NUL。
 * @return 指定第二项故障返回 DISK_ERR，其余 FR_OK。
 */
FRESULT f_readdir(DIR *dir, FILINFO *info)
{
    assert(dir->open);
    memset(info, 0, sizeof(*info));
    if (g_directory_failure && dir->index == 1) return FR_DISK_ERR;
    if (dir->index < 2) strcpy(info->fname, dir->index ? "second" : "first");
    dir->index++;
    return FR_OK;
}

/**
 * @brief 记录目录关闭并支持失败传播验证。
 * @param[in,out] dir 必需打开目录，成功使其失效。
 * @return 故障模式返回 DISK_ERR，其余 FR_OK。
 */
FRESULT f_closedir(DIR *dir)
{
    assert(dir->open);
    g_dir_closed++;
    if (g_closedir_failure) return FR_DISK_ERR;
    dir->open = 0;
    return FR_OK;
}

/**
 * @brief 重置模型卷和故障开关，模拟 BSP 故障恢复后重新启用卷。
 * @post 原生遗留 share 记录清除，生产拥有型分配保持原状供泄漏断言。
 */
static void reset_volume(void)
{
    g_length = 70000;
    g_last_path[0] = '\0';
    g_open_calls = g_close_calls = g_read_calls = g_mutations = 0;
    g_unlink_calls = g_sync_dirty = g_dir_closed = 0;
    g_share_held = g_close_failure = g_short_read = g_short_write = 0;
    g_read_failure = g_directory_failure = g_closedir_failure = 0;
    g_parent_missing = g_existing_file = 0;
    g_fail_at = 0;
}

/**
 * @brief 验证 16 位 UINT 上的大文件分块、短读、OOM 和输出清理。
 * @param[in] fs 借用存活端口。
 * @post 所有成功缓冲释放，失败保持空输出，端口对象为唯一生产分配。
 */
static void test_reads(cc_filesystem_t *fs)
{
    reset_volume();
    char *text = NULL;
    expect_result(fs->vtable->read_text(fs->self, "/sdcard/data", &text), CC_OK);
    assert(text && strlen(text) == 70000 && g_read_calls == 2);
    assert(text[65535] == 'a' + 65535 % 26 && text[70000] == '\0');
    model_free(text);
    void *data = NULL;
    size_t size = 0;
    g_read_calls = 0;
    expect_result(fs->vtable->read_bytes(fs->self, "/sdcard/data", &data, &size), CC_OK);
    assert(size == 70000 && g_read_calls == 2);
    model_free(data);
    g_short_read = 1;
    text = (char *)1;
    expect_result(fs->vtable->read_text(fs->self, "/sdcard/data", &text), CC_ERR_PLATFORM);
    assert(!text);
    data = (void *)1; size = 99;
    expect_result(fs->vtable->read_bytes(fs->self, "/sdcard/data", &data, &size), CC_ERR_PLATFORM);
    assert(!data && !size);
    g_short_read = 0;
    g_read_failure = 1;
    expect_result(fs->vtable->read_text(fs->self, "/sdcard/data", &text), CC_ERR_PLATFORM);
    g_read_failure = 0;
    g_fail_at = g_alloc_calls + 1;
    expect_result(fs->vtable->read_text(fs->self, "/sdcard/data", &text), CC_ERR_OUT_OF_MEMORY);
    assert(!text);
    g_fail_at = 0;
    g_length = 256UL * 1024UL + 1;
    expect_result(fs->vtable->read_text(fs->self, "/sdcard/data", &text), CC_ERR_INVALID_ARGUMENT);
    g_length = 0;
    expect_result(fs->vtable->read_bytes(fs->self, "/sdcard/data", &data, &size), CC_OK);
    assert(data && !size);
    model_free(data);
    data = (void *)1;
    expect_result(fs->vtable->read_bytes(fs->self, "data", &data, NULL), CC_ERR_INVALID_ARGUMENT);
    assert(!data);
    unsigned char tail[8]; size = 99; g_length = 40;
    expect_result(fs->vtable->read_tail(fs->self, "data", tail, sizeof(tail), &size), CC_OK);
    assert(size == sizeof(tail) && tail[0] == 'a' + 32 % 26);
    assert(g_owned_count == 1);
}

/**
 * @brief 验证根先映射、完整组件和 LFΝ/SFN 各自实际输入范围。
 * @param[in] fs 借用存活端口。
 * @post 合法操作只把预期原生路径交给模型，拒绝输入不调用 f_open。
 */
static void test_relative_paths(cc_filesystem_t *fs)
{
    reset_volume();
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/cclaw", "workspace/secret", "x"), CC_OK);
    assert(strcmp(g_last_path, "0:/cclaw/workspace/secret") == 0);
    char *text = NULL;
    g_length = 3;
    expect_result(fs->vtable->read_relative_text(fs->self, "/sdcard/cclaw", "workspace/secret", &text), CC_OK);
    assert(strcmp(g_last_path, "0:/cclaw/workspace/secret") == 0);
    model_free(text);
    expect_result(fs->vtable->write_relative_bytes(fs->self, "/sdcard/cclaw", "workspace/raw", "xy", 2), CC_OK);
    assert(strcmp(g_last_path, "0:/cclaw/workspace/raw") == 0);
    unsigned before = g_open_calls;
    const char *bad[] = {"../x", "a/../x", "a//x", "a/", "/x", "a\\x", "\1ignored/x", "... /x"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        text = (char *)1;
        expect_result(fs->vtable->read_relative_text(fs->self, "/sdcard/root", bad[i], &text), CC_ERR_PERMISSION_DENIED);
        assert(!text);
    }
    expect_result(fs->vtable->write_relative_text(fs->self, "", "x", "x"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/\1root", "x", "x"), CC_ERR_PERMISSION_DENIED);
    assert(before == g_open_calls);
#if FF_USE_LFN
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/root", "a b/foo./x", "x"), CC_OK);
    assert(strcmp(g_last_path, "0:/root/a b/foo./x") == 0);
#else
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/root", "record name/file", "x"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/root", ".. /file", "x"), CC_ERR_PERMISSION_DENIED);
    expect_result(fs->vtable->write_relative_text(fs->self, "/sdcard/root dir", "x", "x"), CC_ERR_PERMISSION_DENIED);
    assert(before == g_open_calls);
#endif
    expect_result(fs->vtable->write_text(fs->self, "/sdcard_other/file", "x"), CC_OK);
    assert(strcmp(g_last_path, "0:/sdcard_other/file") == 0);
    expect_result(fs->vtable->write_text(fs->self, "/sdcard/cclaw/workspace/file", "x"), CC_OK);
    assert(strcmp(g_last_path, "0:/file") == 0);
    char long_path[300]; memset(long_path, 'a', sizeof(long_path) - 1); long_path[299] = '\0';
    text = (char *)1;
    expect_result(fs->vtable->read_relative_text(fs->self, "/sdcard/root", long_path, &text), CC_ERR_INVALID_ARGUMENT);
    assert(!text && g_owned_count == 1);
}

/**
 * @brief 验证长度拒绝发生于副作用前，短写和 close 故障不被成功掩盖。
 * @param[in] fs 借用存活端口。
 * @post 故障卷经模型 BSP 恢复，所有成功写入完成脏状态同步。
 */
static void test_writes_and_close(cc_filesystem_t *fs)
{
    reset_volume();
    expect_result(fs->vtable->write_bytes(fs->self, "data", "x", 65536), CC_ERR_LIMIT_EXCEEDED);
    expect_result(fs->vtable->append_bytes(fs->self, "data", "x", 65536), CC_ERR_LIMIT_EXCEEDED);
    assert(!g_open_calls && !g_mutations);
    expect_result(fs->vtable->write_bytes(fs->self, "data", NULL, 1), CC_ERR_INVALID_ARGUMENT);
    g_short_write = 1;
    expect_result(fs->vtable->write_bytes(fs->self, "data", "abc", 3), CC_ERR_PLATFORM);
    assert(g_close_calls == 1 && g_sync_dirty == 1);
    g_short_write = 0;
    expect_result(fs->vtable->append_bytes(fs->self, "data", "abc", 3), CC_OK);
    assert(g_sync_dirty == 2);
    unsigned synced = g_sync_dirty;
    expect_result(fs->vtable->sync_file(fs->self, "data"), CC_OK);
    assert(g_sync_dirty == synced);
    unsigned mutations = g_mutations;
    expect_result(fs->vtable->atomic_replace(fs->self, "temp", "dest"), CC_ERR_UNSUPPORTED);
    assert(g_mutations == mutations && !g_unlink_calls);
    g_close_failure = 1;
    expect_result(fs->vtable->write_text(fs->self, "data", "abc"), CC_ERR_PLATFORM);
    g_close_failure = 0;
#if FF_FS_LOCK
    expect_result(fs->vtable->write_text(fs->self, "data", "abc"), CC_ERR_PLATFORM);
    assert(g_share_held);
#else
    expect_result(fs->vtable->write_text(fs->self, "data", "abc"), CC_OK);
#endif
    reset_volume();
    g_close_failure = 1;
    char *text = (char *)1;
    expect_result(fs->vtable->read_text(fs->self, "data", &text), CC_ERR_PLATFORM);
    assert(!text && g_owned_count == 1);
    reset_volume();
    g_parent_missing = 1;
    expect_result(fs->vtable->write_text(fs->self, "/sdcard/parent/child", "abc"), CC_OK);
    assert(g_open_calls == 2);
}

/**
 * @brief 验证目录部分结果回收、原生关闭故障以及已有文件不能充当目录。
 * @param[in] fs 借用存活端口。
 * @post 所有名称数组释放，失败输出为空，stat 的未知 UTC 时间为零。
 */
static void test_directories(cc_filesystem_t *fs)
{
    reset_volume();
    char **items = NULL;
    size_t count = 0;
    expect_result(fs->vtable->list_dir(fs->self, "dir", &items, &count), CC_OK);
    assert(count == 2 && strcmp(items[0], "first") == 0);
    for (size_t i = 0; i < count; i++) model_free(items[i]);
    model_free(items);
    for (size_t failure = 1; failure <= 4; failure++) {
        g_fail_at = g_alloc_calls + failure;
        items = (char **)1; count = 9;
        expect_result(fs->vtable->list_dir(fs->self, "dir", &items, &count), CC_ERR_OUT_OF_MEMORY);
        assert(!items && !count && g_owned_count == 1);
        g_fail_at = 0;
    }
    g_directory_failure = 1;
    expect_result(fs->vtable->list_dir(fs->self, "dir", &items, &count), CC_ERR_PLATFORM);
    assert(!items && !count && g_owned_count == 1);
    g_directory_failure = 0; g_closedir_failure = 1;
    expect_result(fs->vtable->list_dir(fs->self, "dir", &items, &count), CC_ERR_PLATFORM);
    assert(!items && !count && g_owned_count == 1);
    g_closedir_failure = 0; g_existing_file = 1;
    expect_result(fs->vtable->make_dir(fs->self, "/sdcard/file/child"), CC_ERR_PLATFORM);
    g_parent_missing = 1;
    expect_result(fs->vtable->write_text(fs->self, "/sdcard/file/child", "x"), CC_ERR_PLATFORM);
    g_parent_missing = 0;
    cc_filesystem_stat_t stat;
    expect_result(fs->vtable->stat(fs->self, "file", &stat), CC_OK);
    assert(stat.type == CC_FS_ENTRY_FILE && stat.byte_size == g_length && !stat.modified_ms);
    expect_result(fs->vtable->stat(fs->self, "missing", &stat), CC_OK);
    assert(stat.type == CC_FS_ENTRY_MISSING);
    int exists = 9;
    expect_result(fs->vtable->exists(fs->self, "missing", &exists), CC_OK);
    assert(!exists);
    expect_result(fs->vtable->sync_dir(fs->self, "dir"), CC_ERR_UNSUPPORTED);
}
#else
/**
 * @brief 验证未编入 FatFs 时的零能力、空输出和无副作用错误。
 * @param[in] fs 借用存活占位端口。
 * @post 所有调用保持零能力，未创建任何文件数据。
 */
static void test_disabled(cc_filesystem_t *fs)
{
    assert(!fs->capabilities && !fs->vtable->read_bytes && !fs->vtable->read_tail);
    char *text = (char *)1;
    expect_result(fs->vtable->read_text(fs->self, "data", &text), CC_ERR_PLATFORM);
    assert(!text);
    expect_result(fs->vtable->write_text(fs->self, "data", "x"), CC_ERR_PLATFORM);
    expect_result(fs->vtable->write_bytes(fs->self, "data", "x", 1), CC_ERR_PLATFORM);
    int exists = 9;
    expect_result(fs->vtable->exists(fs->self, "data", &exists), CC_OK);
    assert(!exists);
    char **items = (char **)1;
    size_t count = 9;
    expect_result(fs->vtable->list_dir(fs->self, "dir", &items, &count), CC_ERR_PLATFORM);
    assert(!items && !count);
    expect_result(fs->vtable->make_dir(fs->self, "dir"), CC_ERR_PLATFORM);
    expect_result(fs->vtable->remove(fs->self, "data"), CC_ERR_PLATFORM);
}
#endif

/**
 * @brief 执行编译分支对应的生产适配器契约回归。
 * @return 全部断言通过返回零，失败由断言终止。
 * @note 同一物理源分别编译 disabled、LFN/share 与 SFN/no-share 变体，未替代真实 BSP、磁盘和 FatFs 内部锁验证。
 */
int main(void)
{
    expect_result(model_get_default(NULL), CC_ERR_INVALID_ARGUMENT);
    cc_filesystem_t fs;
    memset(&fs, 0xa5, sizeof(fs));
    g_fail_at = g_alloc_calls + 1;
    expect_result(model_get_default(&fs), CC_ERR_OUT_OF_MEMORY);
    assert(!fs.self && !fs.vtable && !fs.size && !fs.version && !fs.capabilities);
    g_fail_at = 0;
    expect_result(model_get_posix(&fs), CC_OK);
    assert(fs.self && fs.vtable && g_owned_count == 1);
#if CCLAW_FREERTOS_ENABLE_FATFS
    assert((fs.capabilities & CC_FS_CAP_ROOT_SCOPED) && !(fs.capabilities & CC_FS_CAP_ATOMIC_REPLACE));
    test_reads(&fs);
    test_relative_paths(&fs);
    test_writes_and_close(&fs);
    test_directories(&fs);
#else
    test_disabled(&fs);
#endif
    fs.vtable->destroy(fs.self);
    assert(!g_owned_count);
    return 0;
}

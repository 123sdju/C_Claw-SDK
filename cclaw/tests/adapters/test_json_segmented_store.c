#include "cc/core/cc_media.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_platform.h"
#include "cc/ports/cc_session_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CC_PLATFORM == CC_PLATFORM_WINDOWS
#include <direct.h>
#include <sys/stat.h>
#include <windows.h>
#define cc_mkdir(path) _mkdir(path)
#define cc_rmdir(path) _rmdir(path)
#define cc_stat _stat
typedef struct _stat cc_stat_t;
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define cc_mkdir(path) mkdir(path, 0755)
#define cc_rmdir(path) rmdir(path)
#define cc_stat stat
typedef struct stat cc_stat_t;
#endif

#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & S_IFDIR) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode) (((mode) & S_IFREG) == S_IFREG)
#endif

/*
 * 函数 cc_json_segmented_store_create：声明 cclaw/tests/adapters/test_json_segmented_store.c 中的 json segmented store create 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
extern cc_result_t cc_json_segmented_store_create(
    const char *root_path,
    size_t segment_bytes,
    const char *media_dir,
    size_t max_base64_bytes,
    cc_session_store_t *out_store
);

/*
 * 函数 path_is_dir：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 path is dir 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int path_is_dir(const char *path)
{
    cc_stat_t st;
    return path && cc_stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * 函数 path_is_file：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 path is file 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int path_is_file(const char *path)
{
    cc_stat_t st;
    return path && cc_stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/*
 * 函数 join_path：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 join path 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static char *join_path(const char *a, const char *b)
{
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    char *out = malloc(alen + blen + 2);
    if (!out) return NULL;
    memcpy(out, a, alen);
    out[alen] = '/';
    memcpy(out + alen + 1, b, blen);
    out[alen + blen + 1] = '\0';
    return out;
}

/*
 * 函数 remove_tree_best_effort：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 remove tree best effort 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static void remove_tree_best_effort(const char *path)
{
    if (!path) return;
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(path, "*");
    if (pattern) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                char *child = join_path(path, fd.cFileName);
                if (!child) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    remove_tree_best_effort(child);
                } else {
                    remove(child);
                }
                free(child);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
        free(pattern);
    }
#else
    DIR *dir = opendir(path);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            char *child = join_path(path, ent->d_name);
            if (!child) continue;
            if (path_is_dir(child)) remove_tree_best_effort(child);
            else remove(child);
            free(child);
        }
        closedir(dir);
    }
#endif
    cc_rmdir(path);
}

/*
 * 函数 write_file：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 write file 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(data);
    int ok = fwrite(data, 1, len, f) == len;
    ok = fclose(f) == 0 && ok;
    return ok;
}

/*
 * 函数 find_first_session_dir：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 find first session dir 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static char *find_first_session_dir(const char *root)
{
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(root, "*");
    if (!pattern) return NULL;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char *dir = join_path(root, fd.cFileName);
        FindClose(h);
        return dir;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return NULL;
#else
    DIR *dir = opendir(root);
    if (!dir) return NULL;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *child = join_path(root, ent->d_name);
        if (child && path_is_dir(child)) {
            closedir(dir);
            return child;
        }
        free(child);
    }
    closedir(dir);
    return NULL;
#endif
}

/*
 * 函数 count_files_in_dir：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 count files in dir 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int count_files_in_dir(const char *path)
{
    int count = 0;
#if CC_PLATFORM == CC_PLATFORM_WINDOWS
    char *pattern = join_path(path, "*");
    if (!pattern) return 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) count++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char *child = join_path(path, ent->d_name);
        if (child && path_is_file(child)) count++;
        free(child);
    }
    closedir(dir);
#endif
    return count;
}

/*
 * 函数 segment_contains：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 segment contains 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int segment_contains(const char *session_dir, const char *needle)
{
    for (int i = 1; i < 32; i++) {
        char name[32];
        snprintf(name, sizeof(name), "events-%06d.jsonl", i);
        char *path = join_path(session_dir, name);
        if (!path) return 0;
        if (!path_is_file(path)) {
            free(path);
            break;
        }
        FILE *f = fopen(path, "rb");
        free(path);
        if (!f) return 0;
        char buf[512];
        while (fgets(buf, sizeof(buf), f)) {
            if (strstr(buf, needle)) {
                fclose(f);
                return 1;
            }
        }
        fclose(f);
    }
    return 0;
}

/*
 * 函数 segment_file_count：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 segment file count 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static int segment_file_count(const char *session_dir)
{
    int count = 0;
    for (int i = 1; i < 32; i++) {
        char name[32];
        snprintf(name, sizeof(name), "events-%06d.jsonl", i);
        char *path = join_path(session_dir, name);
        if (!path) return count;
        int exists = path_is_file(path);
        free(path);
        if (!exists) break;
        count++;
    }
    return count;
}

/*
 * 函数 cleanup_messages：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 cleanup messages 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static void cleanup_messages(cc_message_t *messages, size_t count)
{
    for (size_t i = 0; i < count; i++) cc_message_cleanup(&messages[i]);
    free(messages);
}

/*
 * 函数 cleanup_sessions：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 cleanup sessions 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
static void cleanup_sessions(cc_session_t *sessions, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(sessions[i].id);
        free(sessions[i].name);
        free(sessions[i].workspace_dir);
        free(sessions[i].model);
        free(sessions[i].created_at);
        free(sessions[i].updated_at);
    }
    free(sessions);
}

/*
 * 函数 main：实现 cclaw/tests/adapters/test_json_segmented_store.c 中的 main 相关逻辑。
 *
 * 注释用于标明函数职责边界；参数校验、资源所有权、并发限制和错误码语义以函数体
 * 及对应头文件契约为准，后续维护应在修改行为时同步更新这里和项目文档。
 */
int main(void)
{
    const char *root = "runtime/data/test_json_segmented_store";
    const char *session_id = "segmented_session";
    const char *video_src = "runtime/data/test_segmented_video.tmp";
    const char *file_src = "runtime/data/test_segmented_file.tmp";

    remove_tree_best_effort(root);
    remove(video_src);
    remove(file_src);
    cc_mkdir("runtime");
    cc_mkdir("runtime/data");
    if (!write_file(video_src, "video") || !write_file(file_src, "plain")) return 1;

    cc_session_store_t store = {0};
    cc_result_t rc = cc_json_segmented_store_create(root, 256, "media", 1024, &store);
    if (rc.code != CC_OK) return 2;
    rc = store.vtable->create_session(store.self, session_id, ".");
    if (rc.code != CC_OK) return 3;

    char large_text[360];
    memset(large_text, 'x', sizeof(large_text) - 1);
    large_text[sizeof(large_text) - 1] = '\0';
    for (int i = 0; i < 4; i++) {
        char id[16];
        snprintf(id, sizeof(id), "m%d", i);
        cc_message_t *msg = NULL;
        rc = cc_message_create_text(id, session_id, CC_ROLE_USER, large_text, NULL, &msg);
        if (rc.code != CC_OK || !msg) return 4;
        cc_session_record_t record = {
            .type = CC_SESSION_RECORD_MESSAGE,
            .session_id = session_id,
            .data.message = msg,
        };
        rc = store.vtable->append_records(store.self, &record, 1);
        cc_message_destroy(msg);
        if (rc.code != CC_OK) return 5;
    }

    cc_media_artifact_t video = {
        .id = "video_1",
        .kind = CC_MEDIA_VIDEO,
        .mime = "video/mp4",
        .path = (char *)video_src
    };
    cc_media_artifact_t file = {
        .id = "file_1",
        .kind = CC_MEDIA_FILE,
        .mime = "application/octet-stream",
        .path = (char *)file_src
    };
    cc_content_parts_t media_parts;
    cc_content_parts_init(&media_parts);
    rc = cc_content_parts_append_text(&media_parts, "media paths", CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&media_parts, &video, CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&media_parts, &file, CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 6;
    cc_message_t *media_msg = NULL;
    rc = cc_message_create_parts("m_media", session_id, CC_ROLE_USER, &media_parts, NULL, &media_msg);
    if (rc.code != CC_OK || !media_msg) return 7;
    cc_session_record_t media_record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session_id,
        .data.message = media_msg,
    };
    rc = store.vtable->append_records(store.self, &media_record, 1);
    if (rc.code != CC_OK) return 8;

    cc_media_artifact_t audio = {
        .id = "audio_1",
        .kind = CC_MEDIA_AUDIO,
        .mime = "audio/wav",
        .data_base64 = "YXVkaW8="
    };
    cc_tool_result_t tool_result = {0};
    tool_result.ok = 1;
    rc = cc_tool_result_add_artifact(&tool_result, &audio);
    if (rc.code != CC_OK) return 9;
    cc_session_record_t tool_record = {
        .type = CC_SESSION_RECORD_TOOL_RESULT,
        .session_id = session_id,
        .data.tool_result = {
            .tool_call_id = "tool_audio",
            .result = &tool_result,
        },
    };
    rc = store.vtable->append_records(store.self, &tool_record, 1);
    cc_tool_result_cleanup(&tool_result);
    if (rc.code != CC_OK) return 10;

    cc_media_artifact_t image = {
        .id = "image_1",
        .kind = CC_MEDIA_IMAGE,
        .mime = "image/png",
        .data_base64 = "aW1hZ2U=",
        .width = 2,
        .height = 3
    };
    cc_content_parts_t image_parts;
    cc_content_parts_init(&image_parts);
    rc = cc_content_parts_append_text(&image_parts, "image", CC_CONTENT_PART_INPUT);
    if (rc.code == CC_OK) rc = cc_content_parts_append_artifact(&image_parts, &image, CC_CONTENT_PART_INPUT);
    if (rc.code != CC_OK) return 11;
    cc_message_t *image_msg = NULL;
    rc = cc_message_create_parts("m_image", session_id, CC_ROLE_USER, &image_parts, NULL, &image_msg);
    if (rc.code != CC_OK || !image_msg) return 12;
    cc_session_record_t image_record = {
        .type = CC_SESSION_RECORD_MESSAGE,
        .session_id = session_id,
        .data.message = image_msg,
    };
    rc = store.vtable->append_records(store.self, &image_record, 1);
    if (rc.code != CC_OK) return 13;
    if (!image_msg->content.items[1].artifact.data_base64 ||
        strcmp(image_msg->content.items[1].artifact.data_base64, "aW1hZ2U=") != 0) {
        return 14;
    }

    char *session_dir = find_first_session_dir(root);
    if (!session_dir) return 15;
    if (segment_file_count(session_dir) < 2) return 16;
    if (segment_contains(session_dir, "data_base64")) return 17;
    if (!segment_contains(session_dir, "media/img/user") ||
        !segment_contains(session_dir, "media/audio/tool") ||
        segment_contains(session_dir, video_src) ||
        segment_contains(session_dir, file_src)) {
        return 17;
    }

    char *img_dir = join_path(session_dir, "media/img/user");
    char *audio_dir = join_path(session_dir, "media/audio/tool");
    char *video_dir = join_path(session_dir, "media/video/user");
    char *file_dir = join_path(session_dir, "media/file/user");
    if (!img_dir || !audio_dir || !video_dir || !file_dir) return 18;
    if (count_files_in_dir(img_dir) != 1 ||
        count_files_in_dir(audio_dir) != 1 ||
        count_files_in_dir(video_dir) != 1 ||
        count_files_in_dir(file_dir) != 1) {
        return 19;
    }

    cc_message_t *messages = NULL;
    size_t message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 0, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 6 ||
        strcmp(messages[0].id, "m0") != 0 ||
        strcmp(messages[message_count - 1].id, "m_image") != 0) {
        return 20;
    }
    cleanup_messages(messages, message_count);

    messages = NULL;
    message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 2, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 2 ||
        strcmp(messages[0].id, "m_media") != 0 ||
        strcmp(messages[1].id, "m_image") != 0 ||
        !messages[1].content.items[1].artifact.data_base64 ||
        strcmp(messages[1].content.items[1].artifact.data_base64, "aW1hZ2U=") != 0) {
        return 21;
    }
    cleanup_messages(messages, message_count);

    cc_session_t *sessions = NULL;
    size_t session_count = 0;
    rc = store.vtable->list_sessions(store.self, &sessions, &session_count);
    if (rc.code != CC_OK || session_count != 1 || strcmp(sessions[0].id, session_id) != 0) return 22;
    cleanup_sessions(sessions, session_count);

    rc = store.vtable->clear_session(store.self, session_id);
    if (rc.code != CC_OK) return 23;
    if (count_files_in_dir(img_dir) != 0 || count_files_in_dir(audio_dir) != 0) return 24;
    messages = NULL;
    message_count = 0;
    rc = store.vtable->load_messages(store.self, session_id, 0, &messages, &message_count);
    if (rc.code != CC_OK || message_count != 0) return 25;
    cleanup_messages(messages, message_count);

    free(img_dir);
    free(audio_dir);
    free(video_dir);
    free(file_dir);
    free(session_dir);
    cc_message_destroy(media_msg);
    cc_message_destroy(image_msg);
    cc_content_parts_cleanup(&media_parts);
    cc_content_parts_cleanup(&image_parts);
    store.vtable->destroy(store.self);
    remove_tree_best_effort(root);
    remove(video_src);
    remove(file_src);
    return 0;
}

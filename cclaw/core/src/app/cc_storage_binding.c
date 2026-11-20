#include "cc/app/cc_storage_binding.h"
#include "cc/core/cc_id.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORAGE_ID_FILE "storage.id"
#define STORAGE_ID_TEMP "storage.id.tmp"
#define STORAGE_ID_BACKUP "storage.id.bak"
#define STORAGE_ID_PREFIX "storage-"
#define STORAGE_ID_LENGTH 44U

/**
 * @brief 检查绝对 FAT 路径的词法形式并取得去掉单个尾斜杠后的长度。
 * @param[in] path 调用期稳定 NUL 文本，可为 NULL。
 * @param[out] out_length 必填长度槽，成功才写入，不修改原路径。
 * @return 合法为 1，空值、非绝对、重复分隔、点段、尾点、禁用字节或长度达到 256 为 0。
 * @note 只接受可打印 ASCII 的明确名称，根路径合法；文件系统别名和符号链接仍由外层约束。
 */
static int canonical_fat_path(const char *path, size_t *out_length)
{
    if (!path || path[0] != '/') return 0;
    size_t length = strlen(path);
    if (!length || length >= 256U) return 0;
    if (length > 1U && path[length - 1U] == '/') length--;
    if (length == 1U) {
        if (path[1] != '\0') return 0;
        *out_length = length;
        return 1;
    }
    size_t segment = 1U;
    for (size_t i = 1U; i <= length; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (i == length || ch == '/') {
            size_t n = i - segment;
            if (!n || (n == 1U && path[segment] == '.') ||
                (n == 2U && path[segment] == '.' && path[segment + 1U] == '.') ||
                path[i - 1U] == '.') return 0;
            segment = i + 1U;
        } else if (ch < 0x21U || ch > 0x7eU || strchr("\\:~*?<>|\"", ch)) return 0;
    }
    *out_length = length;
    return 1;
}

/**
 * @brief 为 FAT 路径比较折叠大写 ASCII 字母。
 * @param[in] c 单个字节值。
 * @return A 至 Z 映射小写，其余字节保持原值。
 */
static char fold_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}

/**
 * @brief 判断两个已通过词法校验的路径是否存在父子或相等关系。
 * @param[in] parent 稳定父路径借用。
 * @param[in] parent_length 校验器返回的有效字节长度。
 * @param[in] child 稳定子路径借用。
 * @param[in] child_length 校验器返回的有效字节长度。
 * @return 忽略 ASCII 大小写后相等或分段边界上的前缀为 1，否则为 0。
 */
static int path_prefix(const char *parent, size_t parent_length, const char *child, size_t child_length)
{
    if (parent_length == 1U) return child[0] == '/';
    if (parent_length > child_length) return 0;
    for (size_t i=0; i<parent_length; i++) if (fold_ascii(parent[i]) != fold_ascii(child[i])) return 0;
    return parent_length == child_length || child[parent_length] == '/';
}

/**
 * @brief 验证两个 FAT 绝对路径并拒绝重叠子树。
 * @param[in] workspace 调用期借用工作路径，须为明确 ASCII FAT 名称。
 * @param[in] private_directory 调用期借用保护目录，规则同 workspace。
 * @return 合法且分离为 CC_OK，词法错误为 INVALID_ARGUMENT，任向重叠为 PERMISSION_DENIED。
 * @note 无 IO、分配或内部锁，当前设备恢复装配使用此路径隔离检查。
 */
cc_result_t cc_storage_binding_validate_fat_paths(const char *workspace, const char *private_directory)
{
    size_t a=0,b=0;
    if (!canonical_fat_path(workspace,&a) || !canonical_fat_path(private_directory,&b))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Storage paths need unambiguous absolute FAT names");
    if (path_prefix(workspace,a,private_directory,b) || path_prefix(private_directory,b,workspace,a))
        return cc_result_error(CC_ERR_PERMISSION_DENIED,"Workspace overlaps protected recovery storage");
    return cc_result_ok();
}

/**
 * @brief 单独验证一个 FAT 绝对路径的明确拼写。
 * @param[in] path 调用期稳定借用文本，可为 NULL。
 * @return 合法为 CC_OK，否则为 INVALID_ARGUMENT，动态诊断交调用者释放。
 */
cc_result_t cc_storage_binding_validate_fat_path(const char *path)
{
    size_t length=0;
    return canonical_fat_path(path,&length) ? cc_result_ok() :
        cc_result_error(CC_ERR_INVALID_ARGUMENT,"Storage path needs an unambiguous absolute FAT name");
}

/**
 * @brief 检查 storage- 前缀和小写 UUID v4 格式。
 * @param[in] identity 稳定 NUL 文本，可为 NULL。
 * @return 恰为 44 字节且版本/变体合法为 1，其余为 0。
 */
static int valid_identity(const char *identity)
{
    if (!identity || strlen(identity) != STORAGE_ID_LENGTH || strncmp(identity, STORAGE_ID_PREFIX, 8) != 0) return 0;
    for (size_t i = 0; i < 36U; i++) {
        char c = identity[8U + i];
        if (i == 8U || i == 13U || i == 18U || i == 23U) { if (c != '-') return 0; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return identity[8U + 14U] == '4' && strchr("89ab", identity[8U + 19U]) != NULL;
}

/**
 * @brief 分配目录与内部固定文件名的斜杠拼接路径。
 * @param[in] directory 非 NULL 稳定目录文本。
 * @param[in] name 非 NULL 内部短文件名常量。
 * @return 调用者 free 的拥有文本，容量不可表达或分配失败为 NULL。
 */
static char *make_path(const char *directory, const char *name)
{
    size_t a = strlen(directory), b = strlen(name);
    if (a > SIZE_MAX - b - 2U) return NULL;
    char *path = malloc(a + b + 2U);
    if (path) snprintf(path, a + b + 2U, "%s/%s", directory, name);
    return path;
}

/**
 * @brief 核对绑定操作需要的完整结构布局、回调及文件系统能力。
 * @param[in] fs 调用期借用文件系统描述符，可为 NULL。
 * @param[in] directory 调用期借用非空目录文本。
 * @param[in] device 调用期借用设备身份端口，可为 NULL。
 * @param[in] for_write 非零时另要求初始写、文件同步及原子替换能力。
 * @return 依赖有效为 CC_OK，读依赖无效为 INVALID_ARGUMENT，缺初始化能力为 UNSUPPORTED。
 * @note 仅检查依赖，不创建目录、不加锁，也不改变设备默认换盘策略。
 */
static cc_result_t validate(cc_filesystem_t *fs, const char *directory,
                            const cc_storage_identity_store_t *device, int for_write)
{
    if (!fs || fs->size < sizeof(*fs) || !fs->vtable || !directory || !directory[0] ||
        !device || device->size < sizeof(*device) || !device->read ||
        fs->vtable->size < offsetof(cc_filesystem_vtable_t, sync_dir) + sizeof(fs->vtable->sync_dir) ||
        !(fs->capabilities & CC_FS_CAP_STAT) || !fs->vtable->stat || !fs->vtable->read_text)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid storage binding dependencies");
    if (for_write && (!device->write_initial ||
        (fs->capabilities & (CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC)) !=
            (CC_FS_CAP_ATOMIC_REPLACE | CC_FS_CAP_FILE_SYNC) ||
        !fs->vtable->write_text || !fs->vtable->remove || !fs->vtable->sync_file ||
        !fs->vtable->atomic_replace || ((fs->capabilities & CC_FS_CAP_DIR_SYNC) && !fs->vtable->sync_dir)))
        return cc_result_error(CC_ERR_UNSUPPORTED, "Storage binding cannot be durably initialized");
    return cc_result_ok();
}

/**
 * @brief 检查主标记及固定临时/备份文件，读取独立媒体身份。
 * @param[in] fs 已通过 validate 的存活文件系统借用。
 * @param[in] directory 非 NULL 稳定目录，调用者独占且挂载保持不变。
 * @param[out] out_identity 必填新槽，先置 NULL，成功标记由调用者 free，缺失保持 NULL。
 * @return 完整有效标记或缺失为 CC_OK，旁路/格式错误为 STORAGE，IO/OOM 原样返回。
 * @note 消费中间成功诊断，不删除主文件或冲突现场。
 */
static cc_result_t inspect_media(cc_filesystem_t *fs, const char *directory, char **out_identity)
{
    *out_identity = NULL;
    const char *names[] = {STORAGE_ID_FILE, STORAGE_ID_TEMP, STORAGE_ID_BACKUP};
    cc_filesystem_stat_t info[3];
    cc_result_t rc = cc_result_ok();
    for (size_t i=0; i<3 && rc.code==CC_OK; i++) {
        char *path=make_path(directory,names[i]);
        if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate storage identity path");
        memset(&info[i],0,sizeof(info[i])); info[i].size=sizeof(info[i]);
        rc=fs->vtable->stat(fs->self,path,&info[i]); free(path);
        if (rc.code == CC_OK) cc_result_free(&rc);
    }
    if (rc.code != CC_OK) return rc;
    if (info[1].type != CC_FS_ENTRY_MISSING || info[2].type != CC_FS_ENTRY_MISSING)
        return cc_result_error(CC_ERR_STORAGE,"Storage identity has an unfinished replacement");
    if (info[0].type == CC_FS_ENTRY_MISSING) return cc_result_ok();
    if (info[0].type != CC_FS_ENTRY_FILE || info[0].byte_size != STORAGE_ID_LENGTH)
        return cc_result_error(CC_ERR_STORAGE,"Storage identity marker is invalid");
    char *path=make_path(directory,STORAGE_ID_FILE);
    if (!path) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate storage identity path");
    rc=fs->vtable->read_text(fs->self,path,out_identity); free(path);
    if (rc.code != CC_OK) {
        free(*out_identity);
        *out_identity = NULL;
        return rc;
    }
    if (!valid_identity(*out_identity)) {
        free(*out_identity); *out_identity=NULL;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"Storage identity marker is invalid");
    }
    return rc;
}

/**
 * @brief 只读比较设备与媒体标记，另输出设备读取/格式失败来源。
 * @param[in] fs 调用期间借用的存活文件系统。
 * @param[in] directory 稳定目录文本，缺失目录按无媒体身份处理。
 * @param[in] device 存活身份端口，read 返回的字符串由本函数消费。
 * @param[out] out_state 必填独立槽，先置零，成功填写五种组合状态之一。
 * @param[out] out_device_identity_failed 可空独立标志槽，先置零，设备读取/格式失败时置 1。
 * @return 成功为 CC_OK，依赖、媒体、IO/OOM 或设备错误原样上报，失败状态保持零。
 * @note caller 全程独占目录和设备端口并保证挂载稳定，内部无锁、不修复或写入身份。
 */
cc_result_t cc_storage_binding_inspect_detailed(cc_filesystem_t *fs, const char *directory,
    const cc_storage_identity_store_t *device, cc_storage_binding_state_t *out_state,
    int *out_device_identity_failed)
{
    if (out_device_identity_failed) *out_device_identity_failed=0;
    if (!out_state) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Null storage binding state");
    *out_state=0;
    cc_result_t rc=validate(fs,directory,device,0);
    if (rc.code != CC_OK) return rc;
    cc_filesystem_stat_t info={.size=sizeof(info)};
    rc=fs->vtable->stat(fs->self,directory,&info);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (info.type != CC_FS_ENTRY_DIRECTORY && info.type != CC_FS_ENTRY_MISSING)
        return cc_result_error(CC_ERR_STORAGE,"Storage binding directory is unavailable");
    char *on_device=NULL, *on_media=NULL;
    rc=device->read(device->self,&on_device);
    if (rc.code == CC_ERR_NOT_FOUND) { free(on_device); on_device=NULL; cc_result_free(&rc); rc=cc_result_ok(); }
    else if (rc.code == CC_OK && !valid_identity(on_device)) {
        if (out_device_identity_failed) *out_device_identity_failed=1;
        cc_result_free(&rc);
        rc=cc_result_error(CC_ERR_STORAGE,"Device storage identity is invalid");
    } else if (rc.code != CC_OK && out_device_identity_failed) *out_device_identity_failed=1;
    if (rc.code == CC_OK && info.type == CC_FS_ENTRY_DIRECTORY) {
        cc_result_free(&rc);
        rc=inspect_media(fs,directory,&on_media);
    }
    if (rc.code == CC_OK) {
        if (on_device && on_media)
            *out_state=strcmp(on_device,on_media)==0 ? CC_STORAGE_BINDING_MATCH : CC_STORAGE_BINDING_MISMATCH;
        else if (on_device) *out_state=CC_STORAGE_BINDING_DEVICE_ONLY;
        else if (on_media) *out_state=CC_STORAGE_BINDING_MEDIA_ONLY;
        else *out_state=CC_STORAGE_BINDING_UNBOUND;
    }
    free(on_device); free(on_media);
    return rc;
}

/**
 * @brief 使用详细检查取得设备/媒体身份组合，不接收独立设备错误标志。
 * @param[in] fs 存活文件系统借用，布局与能力要求同详细入口。
 * @param[in] directory 调用期稳定目录借用，允许实际目录缺失。
 * @param[in] device 调用期存活身份端口借用。
 * @param[out] out_state 必填新槽，先置零，成功为组合状态。
 * @return 详细入口结果原样转交，动态诊断由调用者释放。
 * @pre 外层独占目录及设备端口，挂载保持稳定。
 */
cc_result_t cc_storage_binding_inspect(cc_filesystem_t *fs, const char *directory,
                                       const cc_storage_identity_store_t *device,
                                       cc_storage_binding_state_t *out_state)
{
    return cc_storage_binding_inspect_detailed(fs,directory,device,out_state,NULL);
}

/**
 * @brief 仅在双侧身份缺失且目录已存在时，先持久化媒体再写设备身份。
 * @param[in,out] fs 存活文件系统借用，需文件同步和原子替换能力。
 * @param[in] directory 已存在的稳定目录，调用者全程独占且挂载不变。
 * @param[in,out] device 存活身份端口，write_initial 须原子拒绝覆盖已有身份。
 * @param[out] out_device_identity_failed 可空标志槽，先清零，设备读写或核对失败时置 1。
 * @return 成功为 CC_OK，已绑定为 INVALID_STATE，目录/能力/IO/OOM 或核对失败返回对应结果。
 * @note 检查已确认临时/备份缺失，在既有独占前提下仅清理本次已进入写入的临时文件。
 * replace 一旦尝试便保留不确定现场，设备提交失败也可能已安装双方标记。
 * owner 须锁存初始化失败，重启或明确修复后再恢复其身份相关工作。
 */
cc_result_t cc_storage_binding_initialize_detailed(cc_filesystem_t *fs, const char *directory,
    const cc_storage_identity_store_t *device, int *out_device_identity_failed)
{
    if (out_device_identity_failed) *out_device_identity_failed = 0;
    cc_result_t rc = validate(fs, directory, device, 1);
    if (rc.code != CC_OK) return rc;
    cc_filesystem_stat_t info = {.size = sizeof(info)};
    rc = fs->vtable->stat(fs->self, directory, &info);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (info.type != CC_FS_ENTRY_DIRECTORY)
        return cc_result_error(CC_ERR_STORAGE, "Storage identity directory must already exist");

    cc_storage_binding_state_t state = 0;
    rc = cc_storage_binding_inspect_detailed(fs, directory, device, &state, out_device_identity_failed);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    if (state != CC_STORAGE_BINDING_UNBOUND)
        return cc_result_error(CC_ERR_INVALID_STATE, "Storage identity is already present or needs repair");
    char *identity = NULL, *path = NULL, *temp = NULL, *check = NULL;
    int write_attempted = 0;
    int replace_attempted = 0;
    rc = cc_id_generate_uuid_v4(STORAGE_ID_PREFIX, &identity);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        path = make_path(directory, STORAGE_ID_FILE);
        temp = make_path(directory, STORAGE_ID_TEMP);
        if (!path || !temp)
            rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate storage identity paths");
    }
    if (rc.code == CC_OK) {
        write_attempted = 1;
        rc = fs->vtable->write_text(fs->self, temp, identity);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = fs->vtable->sync_file(fs->self, temp);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = fs->vtable->read_text(fs->self, temp, &check);
    }
    if (rc.code == CC_OK && (!check || strcmp(check, identity) != 0)) {
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_STORAGE, "Storage identity verification failed");
    }
    free(check);
    check = NULL;
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        replace_attempted = 1;
        rc = fs->vtable->atomic_replace(fs->self, temp, path);
    }
    if (rc.code == CC_OK && (fs->capabilities & CC_FS_CAP_DIR_SYNC)) {
        cc_result_free(&rc);
        rc = fs->vtable->sync_dir(fs->self, directory);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = inspect_media(fs, directory, &check);
    }
    if (rc.code == CC_OK && (!check || strcmp(check, identity) != 0)) {
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_STORAGE, "Installed storage identity verification failed");
    }
    free(check);
    check = NULL;
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = device->write_initial(device->self, identity);
        if (rc.code != CC_OK && out_device_identity_failed) *out_device_identity_failed = 1;
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = device->read(device->self, &check);
        if (rc.code != CC_OK && out_device_identity_failed) *out_device_identity_failed = 1;
    }
    if (rc.code == CC_OK && (!check || strcmp(check, identity) != 0)) {
        if (out_device_identity_failed) *out_device_identity_failed = 1;
        cc_result_free(&rc);
        rc = cc_result_error(CC_ERR_STORAGE, "Device identity verification failed");
    }
    if (rc.code != CC_OK && write_attempted && !replace_attempted) {
        cc_result_t cleanup = fs->vtable->remove(fs->self, temp);
        cc_result_free(&cleanup);
    }
    free(check);
    free(identity);
    free(path);
    free(temp);
    return rc;
}

/**
 * @brief 初始化双侧身份，不接收独立设备失败标志。
 * @param[in,out] fs 存活文件系统，能力同详细入口。
 * @param[in] directory 已存在目录，调用者全程独占并保持挂载稳定。
 * @param[in,out] device 存活设备身份端口，禁止隐式覆盖旧身份。
 * @return 详细初始化结果原样交付，动态诊断由调用者释放。
 * @note 初始化失败由 owner 锁存并等待重启或明确修复，当前设备默认换盘流程未调用此入口。
 */
cc_result_t cc_storage_binding_initialize(cc_filesystem_t *fs, const char *directory,
                                          const cc_storage_identity_store_t *device)
{
    return cc_storage_binding_initialize_detailed(fs,directory,device,NULL);
}

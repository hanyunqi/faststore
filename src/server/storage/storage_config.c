#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "fastcommon/ini_file_reader.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "sf/sf_global.h"
#include "../server_types.h"
#include "store_path_index.h"
#include "storage_config.h"

static int ini_get_ratio_value(const char *storage_filename,
        IniContext *ini_context, const char *section_name,
        const char *item_name, double *reserved_space,
        const double default_value)
{
    char *value;
    char *last;

    value = iniGetStrValue(section_name, item_name, ini_context);
    if (value == NULL || *value == '\0') {
        *reserved_space = default_value;
    } else {
        double d;
        char *endptr;

        last = value + strlen(value) - 1;
        if (*last != '%') {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, item: %s, value: %s "
                    "is NOT a valid ratio, expect end char: %%",
                    __LINE__, storage_filename, item_name, value);
            return EINVAL;
        }

        d = strtod(value, &endptr);
        if ((endptr != last) || (d <= 0.00001 || d >= 100.00001)) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, item: %s, "
                    "value: %s is NOT a valid ratio",
                    __LINE__, storage_filename, item_name, value);
            return EINVAL;
        }

        *reserved_space = d / 100.00;
    }

    return 0;
}

static int load_one_path(FSStorageConfig *storage_cfg,
        const char *storage_filename, IniContext *ini_context,
        const char *section_name, string_t *path)
{
    int result;
    char *path_str;

    path_str = iniGetStrValue(section_name, "path", ini_context);
    if (path_str == NULL || *path_str == '\0') {
        logError("file: "__FILE__", line: %d, "
                "config file: %s, section: %s, item: path "
                "not exist or is empty", __LINE__, storage_filename,
                section_name);
        return ENOENT;
    }

    if (access(path_str, F_OK) == 0) {
        if (!isDir(path_str)) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, section: %s, item: path, "
                    "%s is NOT a path", __LINE__, storage_filename,
                    section_name, path_str);
            return EINVAL;
        }
    } else {
        result = errno != 0 ? errno : EPERM;
        if (result != ENOENT) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, section: %s, access path %s fail, "
                    "errno: %d, error info: %s", __LINE__, storage_filename,
                    section_name, path_str, result, STRERROR(result));
            return result;
        }

        if (mkdir(path_str, 0775) != 0) {
            result = errno != 0 ? errno : EPERM;
            logError("file: "__FILE__", line: %d, "
                    "mkdir %s fail, errno: %d, error info: %s",
                    __LINE__, path_str, result, STRERROR(result));
            return result;
        }
        
        SF_CHOWN_RETURN_ON_ERROR(path_str, geteuid(), getegid());
    }

    chopPath(path_str);
    path->len = strlen(path_str);
    path->str = (char *)fc_malloc(path->len + 1);
    if (path->str == NULL) {
        return ENOMEM;
    }

    memcpy(path->str, path_str, path->len + 1);
    return 0;
}

int storage_config_calc_path_spaces(FSStoragePathInfo *path_info)
{
    struct statvfs sbuf;
    int64_t total_space;

    if (statvfs(path_info->store.path.str, &sbuf) != 0) {
        logError("file: "__FILE__", line: %d, "
                "statfs path %s fail, errno: %d, error info: %s.",
                __LINE__, path_info->store.path.str, errno, STRERROR(errno));
        return errno != 0 ? errno : EPERM;
    }

    total_space = (int64_t)(sbuf.f_blocks) * sbuf.f_frsize;
    path_info->avail_space = (int64_t)(sbuf.f_bavail) * sbuf.f_frsize;
    path_info->reserved_space.value = total_space *
        path_info->reserved_space.ratio;
    return 0;
}

static int load_paths(FSStorageConfig *storage_cfg,
        const char *storage_filename, IniContext *ini_context,
        const char *section_name_prefix, const char *item_name,
        FSStoragePathArray *parray, const bool required)
{
    int result;
    int count;
    int bytes;
    int i;
    char section_name[64];

    count = iniGetIntValue(NULL, item_name, ini_context, 0);
    if (count <= 0) {
        if (required) {
            logError("file: "__FILE__", line: %d, "
                    "config file: %s, item \"%s\" "
                    "not exist or invalid", __LINE__,
                    storage_filename, item_name);
            return ENOENT;
        } else {
            parray->count = 0;
            return 0;
        }
    }

    bytes = sizeof(FSStoragePathInfo) * count;
    parray->paths = (FSStoragePathInfo *)fc_malloc(bytes);
    if (parray->paths == NULL) {
        return ENOMEM;
    }
    memset(parray->paths, 0, bytes);

    for (i=0; i<count; i++) {
        sprintf(section_name, "%s-%d", section_name_prefix, i + 1);
        if ((result=load_one_path(storage_cfg, storage_filename,
                        ini_context, section_name,
                        &parray->paths[i].store.path)) != 0)
        {
            return result;
        }

        parray->paths[i].write_thread_count = iniGetIntValue(section_name,
                "write_threads", ini_context, storage_cfg->
                write_threads_per_disk);
        if (parray->paths[i].write_thread_count <= 0) {
            parray->paths[i].write_thread_count = 1;
        }

        parray->paths[i].read_thread_count = iniGetIntValue(section_name,
                "read_threads", ini_context, storage_cfg->
                read_threads_per_disk);
        if (parray->paths[i].read_thread_count <= 0) {
            parray->paths[i].read_thread_count = 1;
        }

        parray->paths[i].prealloc_trunks = iniGetIntValue(section_name,
                "prealloc_trunks", ini_context, storage_cfg->
                prealloc_trunks_per_writer);
        if (parray->paths[i].prealloc_trunks <= 0) {
            parray->paths[i].prealloc_trunks = 2;
        }

        if ((result=ini_get_ratio_value(storage_filename, ini_context,
                        section_name, "reserved_space",
                        &parray->paths[i].reserved_space.ratio,
                        storage_cfg->reserved_space_per_disk)) != 0)
        {
            return result;
        }

        if ((result=storage_config_calc_path_spaces(parray->paths + i)) != 0) {
            return result;
        }
    }

    parray->count = count;
    return 0;
}

static int load_global_items(FSStorageConfig *storage_cfg,
        const char *storage_filename, IniContext *ini_context)
{
    int result;
    char *tf_size;
    char *discard_size;
    int64_t trunk_file_size;
    int64_t discard_remain_space_size;

    storage_cfg->fd_cache_capacity_per_read_thread = iniGetIntValue(NULL,
            "fd_cache_capacity_per_read_thread", ini_context, 256);
    if (storage_cfg->fd_cache_capacity_per_read_thread <= 0) {
        storage_cfg->fd_cache_capacity_per_read_thread = 256;
    }

    storage_cfg->object_block.hashtable_capacity = iniGetInt64Value(NULL,
            "object_block_hashtable_capacity", ini_context, 1403641);
    if (storage_cfg->object_block.hashtable_capacity <= 0) {
        logWarning("file: "__FILE__", line: %d, "
                "config file: %s, item \"object_block_hashtable_capacity\": "
                "%"PRId64" is invalid, set to default: %d",
                __LINE__, storage_filename, storage_cfg->
                object_block.hashtable_capacity, 1403641);
        storage_cfg->object_block.hashtable_capacity = 1403641;
    }

    storage_cfg->object_block.shared_locks_count = iniGetIntValue(NULL,
            "object_block_shared_locks_count", ini_context, 163);
    if (storage_cfg->object_block.shared_locks_count <= 0) {
        logWarning("file: "__FILE__", line: %d, "
                "config file: %s, item \"object_block_shared_locks_count\": %d "
                "is invalid, set to default: %d",
                __LINE__, storage_filename, storage_cfg->
                object_block.shared_locks_count, 163);
        storage_cfg->object_block.shared_locks_count = 163;
    }

    storage_cfg->write_threads_per_disk = iniGetIntValue(NULL,
            "write_threads_per_disk", ini_context, 1);
    if (storage_cfg->write_threads_per_disk <= 0) {
        storage_cfg->write_threads_per_disk = 1;
    }

    storage_cfg->read_threads_per_disk = iniGetIntValue(NULL,
            "read_threads_per_disk", ini_context, 1);
    if (storage_cfg->read_threads_per_disk <= 0) {
        storage_cfg->read_threads_per_disk = 1;
    }

    storage_cfg->prealloc_trunks_per_writer = iniGetIntValue(NULL,
            "prealloc_trunks_per_writer", ini_context, 2);
    if (storage_cfg->prealloc_trunks_per_writer <= 0) {
        storage_cfg->prealloc_trunks_per_writer = 2;
    }

    storage_cfg->prealloc_trunk_threads = iniGetIntValue(NULL,
            "prealloc_trunk_threads", ini_context, 1);
    if (storage_cfg->prealloc_trunk_threads <= 0) {
        storage_cfg->prealloc_trunk_threads = 1;
    }

    storage_cfg->max_trunk_files_per_subdir = iniGetIntValue(NULL,
            "max_trunk_files_per_subdir", ini_context, 100);
    if (storage_cfg->max_trunk_files_per_subdir <= 0) {
        storage_cfg->max_trunk_files_per_subdir = 100;
    }

    tf_size = iniGetStrValue(NULL, "trunk_file_size", ini_context);
    if (tf_size == NULL || *tf_size == '\0') {
        trunk_file_size = FS_DEFAULT_TRUNK_FILE_SIZE;
    } else if ((result=parse_bytes(tf_size, 1, &trunk_file_size)) != 0) {
        return result;
    }
    storage_cfg->trunk_file_size = trunk_file_size;

    if (storage_cfg->trunk_file_size < FS_TRUNK_FILE_MIN_SIZE) {
        logWarning("file: "__FILE__", line: %d, "
                "trunk_file_size: %"PRId64" is too small, set to %"PRId64,
                __LINE__, storage_cfg->trunk_file_size,
                (int64_t)FS_TRUNK_FILE_MIN_SIZE);
        storage_cfg->trunk_file_size = FS_TRUNK_FILE_MIN_SIZE;
    } else if (storage_cfg->trunk_file_size > FS_TRUNK_FILE_MAX_SIZE) {
        logWarning("file: "__FILE__", line: %d, "
                "trunk_file_size: %"PRId64" is too large, set to %"PRId64,
                __LINE__, storage_cfg->trunk_file_size,
                (int64_t)FS_TRUNK_FILE_MAX_SIZE);
        storage_cfg->trunk_file_size = FS_TRUNK_FILE_MAX_SIZE;
    }

    discard_size = iniGetStrValue(NULL, "discard_remain_space_size",
            ini_context);
    if (discard_size == NULL || *discard_size == '\0') {
        discard_remain_space_size = FS_DEFAULT_DISCARD_REMAIN_SPACE_SIZE;
    } else if ((result=parse_bytes(discard_size, 1,
                    &discard_remain_space_size)) != 0) {
        return result;
    } else {
    }
    storage_cfg->discard_remain_space_size = discard_remain_space_size;

    if (storage_cfg->discard_remain_space_size <
            FS_DISCARD_REMAIN_SPACE_MIN_SIZE)
    {
        logWarning("file: "__FILE__", line: %d, "
                "discard_remain_space_size: %d is too small, set to %d",
                __LINE__, storage_cfg->discard_remain_space_size,
                FS_DISCARD_REMAIN_SPACE_MIN_SIZE);
        storage_cfg->discard_remain_space_size =
            FS_DISCARD_REMAIN_SPACE_MIN_SIZE;
    } else if (storage_cfg->discard_remain_space_size >
            FS_DISCARD_REMAIN_SPACE_MAX_SIZE)
    {
        logWarning("file: "__FILE__", line: %d, "
                "discard_remain_space_size: %d is too large, set to %d",
                __LINE__, storage_cfg->discard_remain_space_size,
                FS_DISCARD_REMAIN_SPACE_MAX_SIZE);
        storage_cfg->discard_remain_space_size =
            FS_DISCARD_REMAIN_SPACE_MAX_SIZE;
    }

    if ((result=ini_get_ratio_value(storage_filename, ini_context,
                    NULL, "reserved_space_per_disk", &storage_cfg->
                    reserved_space_per_disk, 0.10)) != 0)
    {
        return result;
    }

    if ((result=ini_get_ratio_value(storage_filename, ini_context,
                    NULL, "write_cache_to_hd_on_usage", &storage_cfg->
                    write_cache_to_hd.on_usage, 1.00 - storage_cfg->
                    reserved_space_per_disk)) != 0)
    {
        return result;
    }

    if ((result=get_time_item_from_conf(ini_context,
                    "write_cache_to_hd_start_time", &storage_cfg->
                    write_cache_to_hd.start_time, 0, 0)) != 0)
    {
        return result;
    }
    if ((result=get_time_item_from_conf(ini_context,
                    "write_cache_to_hd_end_time", &storage_cfg->
                    write_cache_to_hd.end_time, 0, 0)) != 0)
    {
        return result;
    }

    if ((result=ini_get_ratio_value(storage_filename, ini_context,
                    NULL, "reclaim_trunks_on_usage", &storage_cfg->
                    reclaim_trunks_on_usage, 0.50)) != 0)
    {
        return result;
    }

    return 0;
}

static int load_from_config_file(FSStorageConfig *storage_cfg,
        const char *storage_filename, IniContext *ini_context)
{
    int result;
    if ((result=load_global_items(storage_cfg, storage_filename,
                    ini_context)) != 0)
    {
        return result;
    }
  
    if ((result=load_paths(storage_cfg, storage_filename, ini_context,
                    "store-path", "store_path_count",
                    &storage_cfg->store_path, true)) != 0)
    {
        return result;
    }

    if ((result=load_paths(storage_cfg, storage_filename, ini_context,
                    "write-cache-path", "write_cache_path_count",
                    &storage_cfg->write_cache, false)) != 0)
    {
        return result;
    }

    return 0;
}

static int load_path_indexes(FSStoragePathArray *parray, const char *caption)
{
    int result;
    char mark[64];
    FSStoragePathInfo *p;
    FSStoragePathInfo *end;

    *mark = '\0';
    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        p->store.index = store_path_index_get(p->store.path.str);
        if (p->store.index < 0) {
            //TODO  generate mark
            if ((result=store_path_index_add(p->store.path.str,
                            mark, &p->store.index)) != 0)
            {
                return result;
            }
        }
    }

    return 0;
}

static void do_set_paths_by_index(FSStorageConfig *storage_cfg,
        FSStoragePathArray *parray)
{
    FSStoragePathInfo *p;
    FSStoragePathInfo *end;

    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        storage_cfg->paths_by_index.paths[p->store.index] = p;
    }
}

static int set_paths_by_index(FSStorageConfig *storage_cfg)
{
    int bytes;

    storage_cfg->paths_by_index.count = storage_cfg->max_store_path_index + 1;
    bytes = sizeof(FSStoragePathInfo *) * storage_cfg->paths_by_index.count;
    storage_cfg->paths_by_index.paths = (FSStoragePathInfo **)fc_malloc(bytes);
    if (storage_cfg->paths_by_index.paths == NULL) {
        return ENOMEM;
    }
    memset(storage_cfg->paths_by_index.paths, 0, bytes);

    do_set_paths_by_index(storage_cfg, &storage_cfg->write_cache);
    do_set_paths_by_index(storage_cfg, &storage_cfg->store_path);
    return 0;
}

static int load_store_path_indexes(FSStorageConfig *storage_cfg,
        const char *storage_filename)
{
    int result;
    int old_count;

    if ((result=store_path_index_init()) != 0) {
        return result;
    }

    old_count = store_path_index_count();

    if ((result=load_path_indexes(&storage_cfg->write_cache,
                    "write cache paths")) != 0)
    {
        return result;
    }
    if ((result=load_path_indexes(&storage_cfg->store_path,
                    "store paths")) != 0)
    {
        return result;
    }

    storage_cfg->max_store_path_index = store_path_index_max();
    if (store_path_index_count() != old_count) {
        result = store_path_index_save();
    }

    if (result == 0) {
        result = set_paths_by_index(storage_cfg);
    }

    logInfo("old_count: %d, new_count: %d, max_store_path_index: %d",
            old_count, store_path_index_count(),
            storage_cfg->max_store_path_index);

    store_path_index_destroy();
    return result;
}

int storage_config_load(FSStorageConfig *storage_cfg,
        const char *storage_filename)
{
    IniContext ini_context;
    int result;

    memset(storage_cfg, 0, sizeof(FSStorageConfig));
    if ((result=iniLoadFromFile(storage_filename, &ini_context)) != 0) {
        logError("file: "__FILE__", line: %d, "
                "load conf file \"%s\" fail, ret code: %d",
                __LINE__, storage_filename, result);
        return result;
    }

    result = load_from_config_file(storage_cfg,
            storage_filename, &ini_context);
    iniFreeContext(&ini_context);
    if (result == 0) {
        result = load_store_path_indexes(storage_cfg, storage_filename);
    }
    return result;
}

static void log_paths(FSStoragePathArray *parray, const char *caption)
{
    FSStoragePathInfo *p;
    FSStoragePathInfo *end;

    logInfo("%s count: %d", caption, parray->count);
    end = parray->paths + parray->count;
    for (p=parray->paths; p<end; p++) {
        logInfo("  path %d: %s, index: %d, write_threads: %d, "
                "read_threads: %d, prealloc_trunks: %d, "
                "reserved_space_ratio: %.2f%%, "
                "avail_space: %"PRId64", reserved_space: %"PRId64,
                (int)(p - parray->paths + 1), p->store.path.str,
                p->store.index, p->write_thread_count,
                p->read_thread_count, p->prealloc_trunks,
                p->reserved_space.ratio * 100.00,
                p->avail_space, p->reserved_space.value);
    }
}

void storage_config_to_log(FSStorageConfig *storage_cfg)
{
    logInfo("storage config, write_threads_per_disk: %d, "
            "read_threads_per_disk: %d, "
            "fd_cache_capacity_per_read_thread: %d, "
            "object_block_hashtable_capacity: %"PRId64", "
            "object_block_shared_locks_count: %d, "
            "prealloc_trunks_per_writer: %d, "
            "prealloc_trunk_threads: %d, "
            "reserved_space_per_disk: %.2f%%, "
            "trunk_file_size: %d MB, "
            "max_trunk_files_per_subdir: %d, "
            "discard_remain_space_size: %d, "
            "write_cache_to_hd: { on_usage: %.2f%%, start_time: %02d:%02d, "
            "end_time: %02d:%02d }, reclaim_trunks_on_usage: %.2f%%",
            storage_cfg->write_threads_per_disk,
            storage_cfg->read_threads_per_disk,
            storage_cfg->fd_cache_capacity_per_read_thread,
            storage_cfg->object_block.hashtable_capacity,
            storage_cfg->object_block.shared_locks_count,
            storage_cfg->prealloc_trunks_per_writer,
            storage_cfg->prealloc_trunk_threads,
            storage_cfg->reserved_space_per_disk * 100.00,
            (int)(storage_cfg->trunk_file_size / (1024 * 1024)),
            storage_cfg->max_trunk_files_per_subdir,
            storage_cfg->discard_remain_space_size,
            storage_cfg->write_cache_to_hd.on_usage * 100.00,
            storage_cfg->write_cache_to_hd.start_time.hour,
            storage_cfg->write_cache_to_hd.start_time.minute,
            storage_cfg->write_cache_to_hd.end_time.hour,
            storage_cfg->write_cache_to_hd.end_time.minute,
            storage_cfg->reclaim_trunks_on_usage * 100.00);

    log_paths(&storage_cfg->write_cache, "write cache paths");
    log_paths(&storage_cfg->store_path, "store paths");
}

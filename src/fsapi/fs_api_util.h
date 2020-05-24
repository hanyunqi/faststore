
#ifndef _FS_API_UTIL_H
#define _FS_API_UTIL_H

#include "fs_api_types.h"

#ifdef __cplusplus
extern "C" {
#endif
#define FSAPI_SET_PATH_FULLNAME(fullname, ctx, path_str) \
    fullname.ns = ctx->ns;    \
    FC_SET_STRING(fullname.path, (char *)path_str)

#define fsapi_lookup_inode(path, inode)  \
    fsapi_lookup_inode_ex(&g_fs_api_ctx, path, inode)

#define fsapi_stat_dentry_by_path(path, dentry)  \
    fsapi_stat_dentry_by_path_ex(&g_fs_api_ctx, path, dentry)

#define fsapi_stat_dentry_by_inode(inode, dentry)  \
    fsapi_stat_dentry_by_inode_ex(&g_fs_api_ctx, inode, dentry)

#define fsapi_stat_dentry_by_pname(parent_inode, name, dentry)  \
    fsapi_stat_dentry_by_pname_ex(&g_fs_api_ctx, parent_inode, name, dentry)

#define fsapi_create_dentry_by_pname(parent_inode, name, mode, dentry)  \
    fsapi_create_dentry_by_pname_ex(&g_fs_api_ctx, \
            parent_inode, name, mode, dentry)

#define fsapi_remove_dentry_by_pname(parent_inode, name)  \
    fsapi_remove_dentry_by_pname_ex(&g_fs_api_ctx, \
            parent_inode, name)

#define fsapi_rename_dentry_by_pname(src_parent_inode, src_name, \
        dest_parent_inode, dest_name, flags)  \
    fsapi_rename_dentry_by_pname_ex(&g_fs_api_ctx, src_parent_inode, \
            src_name, dest_parent_inode, dest_name, flags)

#define fsapi_modify_dentry_stat(inode, attr, flags, dentry)  \
    fsapi_modify_dentry_stat_ex(&g_fs_api_ctx, inode, attr, flags, dentry)

#define fsapi_list_dentry_by_inode(inode, array)  \
    fsapi_list_dentry_by_inode_ex(&g_fs_api_ctx, inode, array)

#define fsapi_alloc_opendir_session()  \
    fsapi_alloc_opendir_session_ex(&g_fs_api_ctx)

#define fsapi_free_opendir_session(session) \
        fsapi_free_opendir_session_ex(&g_fs_api_ctx, session)

#define fsapi_dentry_sys_lock(session, inode, flags, file_size) \
    fsapi_dentry_sys_lock_ex(&g_fs_api_ctx, session, inode, flags, file_size)

static inline int fsapi_lookup_inode_ex(FSAPIContext *ctx,
        const char *path, int64_t *inode)
{
    FDIRDEntryFullName fullname;
    FSAPI_SET_PATH_FULLNAME(fullname, ctx, path);
    logInfo("ns: %.*s, path: %s", fullname.ns.len, fullname.ns.str, path);
    return fdir_client_lookup_inode(ctx->contexts.fdir, &fullname, inode);
}

static inline int fsapi_stat_dentry_by_path_ex(FSAPIContext *ctx,
        const char *path, FDIRDEntryInfo *dentry)
{
    FDIRDEntryFullName fullname;
    FSAPI_SET_PATH_FULLNAME(fullname, ctx, path);
    return fdir_client_stat_dentry_by_path(ctx->contexts.fdir,
            &fullname, dentry);
}

static inline int fsapi_stat_dentry_by_inode_ex(FSAPIContext *ctx,
        const int64_t inode, FDIRDEntryInfo *dentry)
{
    return fdir_client_stat_dentry_by_inode(ctx->contexts.fdir,
            inode, dentry);
}

static inline int fsapi_stat_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t parent_inode, const string_t *name,
        FDIRDEntryInfo *dentry)
{
    FDIRDEntryPName pname;
    FDIR_SET_DENTRY_PNAME_PTR(&pname, parent_inode, name);
    return fdir_client_stat_dentry_by_pname(ctx->contexts.fdir,
            &pname, dentry);
}

static inline int fsapi_create_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t parent_inode, const string_t *name,
        const mode_t mode, FDIRDEntryInfo *dentry)
{
    FDIRDEntryPName pname;
    FDIR_SET_DENTRY_PNAME_PTR(&pname, parent_inode, name);
    return fdir_client_create_dentry_by_pname(ctx->contexts.fdir,
            &ctx->ns, &pname, mode, dentry);
}

int fsapi_remove_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t parent_inode, const string_t *name);

int fsapi_rename_dentry_by_pname_ex(FSAPIContext *ctx,
        const int64_t src_parent_inode, const string_t *src_name,
        const int64_t dest_parent_inode, const string_t *dest_name,
        const int flags);

static inline int fsapi_modify_dentry_stat_ex(FSAPIContext *ctx,
        const int64_t inode, const struct stat *attr, const int64_t flags,
        FDIRDEntryInfo *dentry)
{
    FDIRDEntryStatus stat;
    stat.mode = attr->st_mode;
    stat.gid = attr->st_gid;
    stat.uid = attr->st_uid;
    stat.atime = attr->st_atime;
    stat.ctime = attr->st_ctime;
    stat.mtime = attr->st_mtime;
    stat.size = attr->st_size;
    return fdir_client_modify_dentry_stat(ctx->contexts.fdir,
            &ctx->ns, inode, flags, &stat, dentry);
}

static inline int fsapi_list_dentry_by_inode_ex(FSAPIContext *ctx,
        const int64_t inode, FDIRClientDentryArray *array)
{
    return fdir_client_list_dentry_by_inode(ctx->contexts.fdir, inode, array);
}

static inline FSAPIOpendirSession *fsapi_alloc_opendir_session_ex(
        FSAPIContext *ctx)
{
    return (FSAPIOpendirSession *)fast_mblock_alloc_object(
            &ctx->opendir_session_pool);
}

static inline void fsapi_free_opendir_session_ex(
        FSAPIContext *ctx, FSAPIOpendirSession *session)
{
    fast_mblock_free_object(&ctx->opendir_session_pool, session);
}

static inline int fsapi_dentry_sys_lock_ex(FSAPIContext *ctx,
        FDIRClientSession *session, const int64_t inode, const int flags,
        int64_t *file_size)
{
    int result;
    if ((result=fdir_client_init_session(ctx->contexts.fdir, session)) != 0) {
        return result;
    }
    return fdir_client_dentry_sys_lock(session, inode, flags, file_size);
}

static inline int fsapi_dentry_sys_unlock(FDIRClientSession *session,
        const string_t *ns, const int64_t inode, const bool force,
        const int64_t old_size, const int64_t new_size)
{
    int result;
    result = fdir_client_dentry_sys_unlock_ex(session, ns, inode,
            force, old_size, new_size);
    fdir_client_close_session(session, result != 0);
    return result;
}

#ifdef __cplusplus
}
#endif

#endif

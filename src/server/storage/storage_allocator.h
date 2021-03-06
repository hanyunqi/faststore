
#ifndef _STORAGE_ALLOCATOR_H
#define _STORAGE_ALLOCATOR_H

#include "../../common/fs_types.h"
#include "trunk_id_info.h"
#include "trunk_allocator.h"

typedef struct {
    FSTrunkAllocator *allocators;
    int count;
} FSTrunkAllocatorArray;

typedef struct {
    FSTrunkAllocator **allocators;
    int count;
} FSTrunkAllocatorPtrArray;

typedef struct {
    FSTrunkAllocatorArray all;
    FSTrunkAllocatorPtrArray avail;
} FSStorageAllocatorContext;

typedef struct {
    FSStorageAllocatorContext write_cache;
    FSStorageAllocatorContext store_path;
    FSStorageAllocatorContext *current;
    FSTrunkAllocatorPtrArray allocator_ptr_array; //by store path index
    int64_t current_trunk_id;
} FSStorageAllocatorManager;

#ifdef __cplusplus
extern "C" {
#endif

    extern FSStorageAllocatorManager *g_allocator_mgr;

    int storage_allocator_init();

    int storage_allocator_prealloc_trunk_freelists();

    static inline int storage_allocator_add_trunk_ex(const int path_index,
            const FSTrunkIdInfo *id_info, const int64_t size,
            FSTrunkFileInfo **pp_trunk)
    {
        int result;
        if ((result=trunk_id_info_add(path_index, id_info)) != 0) {
            return result;
        }
        return trunk_allocator_add(g_allocator_mgr->allocator_ptr_array.
                allocators[path_index], id_info, size, pp_trunk);
    }

    static inline int storage_allocator_add_trunk(const int path_index,
            const FSTrunkIdInfo *id_info, const int64_t size)
    {
        return storage_allocator_add_trunk_ex(path_index, id_info, size, NULL);
    }

    static inline int storage_allocator_delete_trunk(const int path_index,
            const FSTrunkIdInfo *id_info)
    {
        int result;
        if ((result=trunk_id_info_delete(path_index, id_info)) != 0) {
            return result;
        }
        return trunk_allocator_delete(g_allocator_mgr->allocator_ptr_array.
                allocators[path_index], id_info->id);
    }

    static inline int storage_allocator_normal_alloc(const uint32_t blk_hc,
            const int size, FSTrunkSpaceInfo *space_info, int *count)
    {
        FSTrunkAllocator **allocator;

        if (g_allocator_mgr->current->avail.count == 0) {
            return ENOENT;
        }

        allocator = g_allocator_mgr->current->avail.allocators +
            blk_hc % g_allocator_mgr->current->avail.count;
        return trunk_allocator_normal_alloc(*allocator, blk_hc,
                size, space_info, count);
    }

    static inline int storage_allocator_reclaim_alloc(const uint32_t blk_hc,
            const int size, FSTrunkSpaceInfo *space_info, int *count)
    {
        FSTrunkAllocator **allocator;

        if (g_allocator_mgr->current->avail.count == 0) {
            return ENOENT;
        }

        allocator = g_allocator_mgr->current->avail.allocators +
            blk_hc % g_allocator_mgr->current->avail.count;
        return trunk_allocator_reclaim_alloc(*allocator, blk_hc,
                size, space_info, count);
    }

    static inline int storage_allocator_add_slice(OBSliceEntry *slice)
    {
        return trunk_allocator_add_slice(g_allocator_mgr->allocator_ptr_array.
                allocators[slice->space.store->index], slice);
    }

    static inline int storage_allocator_delete_slice(OBSliceEntry *slice)
    {
        return trunk_allocator_delete_slice(g_allocator_mgr->allocator_ptr_array.
                allocators[slice->space.store->index], slice);
    }

#ifdef __cplusplus
}
#endif

#endif

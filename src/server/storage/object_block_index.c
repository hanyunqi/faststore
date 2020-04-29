#include <limits.h>
#include <sys/stat.h>
#include "fastcommon/shared_func.h"
#include "fastcommon/logger.h"
#include "fastcommon/uniq_skiplist.h"
#include "sf/sf_global.h"
#include "../server_global.h"
#include "storage_allocator.h"
#include "object_block_index.h"

//TODO fixeme!!!
#define SLICE_ARRAY_FIXED_COUNT     4
//#define SLICE_ARRAY_FIXED_COUNT  64

typedef struct {
    int count;
    OBSharedContext *contexts;
} OBSharedContextArray;

typedef struct {
    int64_t count;
    int64_t capacity;
    OBEntry **buckets;
} OBHashtable;

typedef struct {
    int alloc;
    int count;
    OBSliceEntry **slices;
    OBSliceEntry *fixed[SLICE_ARRAY_FIXED_COUNT];
} OBSlicePtrSmartArray;

static OBSharedContextArray ob_shared_ctx_array = {0, NULL};
static OBHashtable ob_hashtable = {0, 0, NULL};

static int slice_compare(const void *p1, const void *p2)
{
    return ((OBSliceEntry *)p1)->ssize.offset -
        ((OBSliceEntry *)p2)->ssize.offset;
}

static void slice_free_func(void *ptr, const int delay_seconds)
{
    ob_index_free_slice((OBSliceEntry *)ptr);
}

static int init_ob_shared_ctx_array()
{
    int result;
    int bytes;
    const int max_level_count = 12;
    const int alloc_skiplist_once = 8 * 1024;
    const int min_alloc_elements_once = 4;
    const int delay_free_seconds = 0;
    const bool bidirection = true;  //need previous link in level 0
    OBSharedContext *ctx;
    OBSharedContext *end;

    ob_shared_ctx_array.count = STORAGE_CFG.object_block.shared_locks_count;
    bytes = sizeof(OBSharedContext) * ob_shared_ctx_array.count;
    ob_shared_ctx_array.contexts = (OBSharedContext *)malloc(bytes);
    if (ob_shared_ctx_array.contexts == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }

    end = ob_shared_ctx_array.contexts + ob_shared_ctx_array.count;
    for (ctx=ob_shared_ctx_array.contexts; ctx<end; ctx++) {
        if ((result=uniq_skiplist_init_ex2(&ctx->factory, max_level_count,
                        slice_compare, slice_free_func, alloc_skiplist_once,
                        min_alloc_elements_once, delay_free_seconds,
                        bidirection)) != 0)
        {
            return result;
        }

        if ((result=fast_mblock_init_ex1(&ctx->ob_allocator,
                        "ob_entry", sizeof(OBEntry), 16 * 1024,
                        NULL, NULL, false)) != 0)
        {
            return result;
        }

        if ((result=fast_mblock_init_ex1(&ctx->slice_allocator,
                        "slice_entry", sizeof(OBSliceEntry),
                        64 * 1024, NULL, NULL, false)) != 0)
        {
            return result;
        }

        if ((result=init_pthread_lock(&ctx->lock)) != 0) {
            logError("file: "__FILE__", line: %d, "
                    "init_pthread_lock fail, errno: %d, error info: %s",
                    __LINE__, result, STRERROR(result));
            return result;
        }
    }

    return 0;
}

static int init_ob_hashtable()
{
    int bytes;

    ob_hashtable.capacity = STORAGE_CFG.object_block.hashtable_capacity;
    bytes = sizeof(OBEntry *) * ob_hashtable.capacity;
    ob_hashtable.buckets = (OBEntry **)malloc(bytes);
    if (ob_hashtable.buckets == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(ob_hashtable.buckets, 0, bytes);

    return 0;
}

int ob_index_init()
{
    int result;

    if ((result=init_ob_shared_ctx_array()) != 0) {
        return result;
    }

    if ((result=init_ob_hashtable()) != 0) {
        return result;
    }

    return 0;
}

void ob_index_destroy()
{
}

static int compare_block_key(const FSBlockKey *bkey1, const FSBlockKey *bkey2)
{
    int64_t sub;

    sub = bkey1->oid - bkey2->oid;
    if (sub < 0) {
        return -1;
    } else if (sub > 0) {
        return 1;
    }

    sub = bkey1->offset - bkey2->offset;
    if (sub < 0) {
        return -1;
    } else if (sub > 0) {
        return 1;
    }
    return 0;
}

static OBEntry *get_ob_entry(OBSharedContext *ctx, OBEntry **bucket,
        const FSBlockKey *bkey, const bool create_flag)
{
    const int init_level_count = 2;
    OBEntry *previous;
    OBEntry *ob;
    int cmpr;

    if (*bucket == NULL) {
        if (!create_flag) {
            return NULL;
        }
        previous = NULL;
    } else {
        cmpr = compare_block_key(bkey, &(*bucket)->bkey);
        if (cmpr == 0) {
            return *bucket;
        } else if (cmpr < 0) {
            previous = NULL;
        } else {
            previous = *bucket;
            while (previous->next != NULL) {
                cmpr = compare_block_key(bkey, &previous->next->bkey);
                if (cmpr == 0) {
                    return previous->next;
                } else if (cmpr < 0) {
                    break;
                }

                previous = previous->next;
            }
        }

        if (!create_flag) {
            return NULL;
        }
    }

    ob = fast_mblock_alloc_object(&ctx->ob_allocator);
    if (ob == NULL) {
        return NULL;
    }
    ob->slices = uniq_skiplist_new(&ctx->factory, init_level_count);
    if (ob->slices == NULL) {
        fast_mblock_free_object(&ctx->ob_allocator, ob);
        return NULL;
    }

    ob->bkey = *bkey;
    if (previous == NULL) {
        ob->next = *bucket;
        *bucket = ob;
    } else {
        ob->next = previous->next;
        previous->next = ob;
    }
    return ob;
}

static inline int do_delete_slice(OBEntry *ob, OBSliceEntry *slice)
{
    int result;

    if ((result=uniq_skiplist_delete(ob->slices, slice)) != 0) {
        return result;
    }
    return storage_allocator_delete_slice(slice);
}

static inline int do_add_slice(OBEntry *ob, OBSliceEntry *slice)
{
    int result;

    if ((result=uniq_skiplist_insert(ob->slices, slice)) != 0) {
        return result;
    }
    return storage_allocator_add_slice(slice);
}

static inline OBSliceEntry *splice_dup(OBSharedContext *ctx,
        const OBSliceEntry *src)
{
    OBSliceEntry *slice;

    slice = fast_mblock_alloc_object(&ctx->slice_allocator);
    if (slice == NULL) {
        return NULL;
    }

    *slice = *src;
    slice->ref_count = 1;
    return slice;
}

static int add_to_slice_ptr_smart_array(OBSlicePtrSmartArray *array,
        OBSliceEntry *slice)
{
    if (array->alloc <= array->count) {
        int alloc;
        int bytes;
        OBSliceEntry **slices;

        alloc = array->alloc * 2;
        bytes = sizeof(OBSliceEntry *) * alloc;
        slices = (OBSliceEntry **)malloc(bytes);
        if (slices == NULL) {
            logError("file: "__FILE__", line: %d, "
                    "malloc %d bytes fail", __LINE__, bytes);
            return ENOMEM;
        }

        memcpy(slices, array->slices, sizeof(OBSliceEntry *) * array->count);
        if (array->slices != array->fixed) {
            free(array->slices);
        }

        array->alloc = alloc;
        array->slices = slices;
    }

    array->slices[array->count++] = slice;
    return 0;
}

static inline int dup_slice_to_smart_array(OBSharedContext *ctx,
        const OBSliceEntry *src_slice, const int offset,
        const int length, OBSlicePtrSmartArray *array)
{
    OBSliceEntry *new_slice;

    new_slice = splice_dup(ctx, src_slice);
    if (new_slice == NULL) {
        return ENOMEM;
    }

    new_slice->ssize.offset = offset;
    new_slice->ssize.length = length;
    return add_to_slice_ptr_smart_array(array, new_slice);
}

#define INIT_SLICE_PTR_ARRAY(sarray) \
    do {   \
        sarray.count = 0;  \
        sarray.alloc = SLICE_ARRAY_FIXED_COUNT;  \
        sarray.slices = sarray.fixed;  \
    } while (0)

#define FREE_SLICE_PTR_ARRAY(sarray) \
    do { \
        if (sarray.slices != sarray.fixed) { \
            free(sarray.slices);  \
        } \
    } while (0)


static int add_slice(OBSharedContext *ctx, OBEntry *ob, OBSliceEntry *slice)
{
    UniqSkiplistNode *node;
    UniqSkiplistNode *previous;
    OBSliceEntry *curr_slice;
    OBSlicePtrSmartArray add_slice_array;
    OBSlicePtrSmartArray del_slice_array;
    int result;
    int curr_end;
    int slice_end;
    int i;

    node = uniq_skiplist_find_ge_node(ob->slices, (void *)slice);
    if (node == NULL) {
        return do_add_slice(ob, slice);
    }

    INIT_SLICE_PTR_ARRAY(add_slice_array);
    INIT_SLICE_PTR_ARRAY(del_slice_array);

    slice_end = slice->ssize.offset + slice->ssize.length;
    previous = UNIQ_SKIPLIST_LEVEL0_PREV_NODE(node);
    if (previous != ob->slices->top) {
        curr_slice = (OBSliceEntry *)previous->data;
        curr_end = curr_slice->ssize.offset + curr_slice->ssize.length;
        if (curr_end > slice->ssize.offset) {  //overlap
            if ((result=add_to_slice_ptr_smart_array(&del_slice_array,
                            curr_slice)) != 0)
            {
                return result;
            }

            if ((result=dup_slice_to_smart_array(ctx, curr_slice,
                            curr_slice->ssize.  offset, slice->ssize.offset -
                            curr_slice->ssize.offset, &add_slice_array)) != 0)
            {
                return result;
            }

            if (curr_end > slice_end) {
                if ((result=dup_slice_to_smart_array(ctx, curr_slice, slice_end,
                                curr_end - slice_end, &add_slice_array)) != 0)
                {
                    return result;
                }
            }
        }
    }

    do {
        curr_slice = (OBSliceEntry *)node->data;
        if (slice_end <= curr_slice->ssize.offset) {  //not overlap
            break;
        }

        if ((result=add_to_slice_ptr_smart_array(&del_slice_array,
                        curr_slice)) != 0)
        {
            return result;
        }

        curr_end = curr_slice->ssize.offset + curr_slice->ssize.length;
        if (curr_end > slice_end) {
            if ((result=dup_slice_to_smart_array(ctx, curr_slice, slice_end,
                            curr_end - slice_end, &add_slice_array)) != 0)
            {
                return result;
            }

            break;
        }

        node = UNIQ_SKIPLIST_LEVEL0_NEXT_NODE(node);
    } while (node != ob->slices->factory->tail);

    for (i=0; i<del_slice_array.count; i++) {
        do_delete_slice(ob, del_slice_array.slices[i]);
    }
    FREE_SLICE_PTR_ARRAY(del_slice_array);

    for (i=0; i<add_slice_array.count; i++) {
        do_add_slice(ob, add_slice_array.slices[i]);
    }
    FREE_SLICE_PTR_ARRAY(add_slice_array);

    return do_add_slice(ob, slice);
}

int ob_index_add_slice(OBSliceEntry *slice)
{
    OBSharedContext *ctx;
    int64_t bucket_index;
    int result;

    bucket_index = FS_BLOCK_HASH_CODE(slice->ob->bkey) %
        ob_hashtable.capacity;
    ctx = ob_shared_ctx_array.contexts + bucket_index %
        ob_shared_ctx_array.count;

    pthread_mutex_lock(&ctx->lock);
    result = add_slice(ctx, slice->ob, slice);
    pthread_mutex_unlock(&ctx->lock);

    return result;
}

OBSliceEntry *ob_index_alloc_slice(const FSBlockKey *bkey)
{
    OBSharedContext *ctx;
    OBEntry **bucket;
    OBEntry *ob;
    OBSliceEntry *slice;
    int64_t bucket_index;

    bucket_index = FS_BLOCK_HASH_CODE(*bkey) % ob_hashtable.capacity;
    ctx = ob_shared_ctx_array.contexts + bucket_index %
        ob_shared_ctx_array.count;
    bucket = ob_hashtable.buckets + bucket_index;

    pthread_mutex_lock(&ctx->lock);
    ob = get_ob_entry(ctx, bucket, bkey, true);
    if (ob == NULL) {
        slice = NULL;
    } else {
        slice = fast_mblock_alloc_object(&ctx->slice_allocator);
        if (slice != NULL) {
            slice->ob = ob;
            slice->ref_count = 1;
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    return slice;
}

void ob_index_free_slice(OBSliceEntry *slice)
{
    OBSharedContext *ctx;
    int64_t bucket_index;

    logInfo("free slice1: %p, ref_count: %d", slice, __sync_add_and_fetch(&slice->ref_count, 0));
    if (__sync_sub_and_fetch(&slice->ref_count, 1) != 0) {
        return;
    }
    logInfo("free slice2: %p, ref_count: %d", slice, __sync_add_and_fetch(&slice->ref_count, 0));

    bucket_index = FS_BLOCK_HASH_CODE(slice->ob->bkey) %
        ob_hashtable.capacity;
    ctx = ob_shared_ctx_array.contexts + bucket_index %
        ob_shared_ctx_array.count;

    pthread_mutex_lock(&ctx->lock);
    fast_mblock_free_object(&ctx->slice_allocator, slice);
    pthread_mutex_unlock(&ctx->lock);
}

static int add_to_slice_ptr_array(OBSlicePtrArray *array,
        OBSliceEntry *slice)
{
    if (array->alloc <= array->count) {
        int alloc;
        int bytes;
        OBSliceEntry **slices;

        if (array->alloc == 0) {
            alloc = 256;
        } else {
            alloc = array->alloc * 2;
        }
        bytes = sizeof(OBSliceEntry *) * alloc;
        slices = (OBSliceEntry **)malloc(bytes);
        if (slices == NULL) {
            logError("file: "__FILE__", line: %d, "
                    "malloc %d bytes fail", __LINE__, bytes);
            return ENOMEM;
        }

        if (array->slices != NULL) {
            memcpy(slices, array->slices, sizeof(OBSliceEntry *) *
                    array->count);
            free(array->slices);
        }

        array->alloc = alloc;
        array->slices = slices;
    }

    array->slices[array->count++] = slice;
    return 0;
}

static inline int dup_slice_to_array(OBSharedContext *ctx,
        const OBSliceEntry *src_slice, const int offset,
        const int length, OBSlicePtrArray *array)
{
    OBSliceEntry *new_slice;

    new_slice = splice_dup(ctx, src_slice);
    if (new_slice == NULL) {
        return ENOMEM;
    }

    new_slice->ssize.offset = offset;
    new_slice->ssize.length = length;
    return add_to_slice_ptr_array(array, new_slice);
}

/*
static void print_skiplist(OBEntry *ob)
{
    UniqSkiplistIterator it;
    OBSliceEntry *slice;
    int count = 0;

    logInfo("forward iterator:");
    uniq_skiplist_iterator(ob->slices, &it);
    while ((slice=(OBSliceEntry *)uniq_skiplist_next(&it)) != NULL) {

        if (count <= 1) {
            logInfo("%d. slice offset: %d, length: %d, end: %d",
                    count, slice->ssize.offset, slice->ssize.length,
                    slice->ssize.offset + slice->ssize.length);
        }
        ++count;
    }


    {
    UniqSkiplistNode *node;
    logInfo("reverse iterator:");
    node = UNIQ_SKIPLIST_LEVEL0_TAIL_NODE(ob->slices);
    while (node != ob->slices->top) {
        slice = (OBSliceEntry *)node->data;

        --count;
        if (count <= 1) {
            logInfo("%d. slice offset: %d, length: %d, end: %d",
                    count, slice->ssize.offset, slice->ssize.length,
                    slice->ssize.offset + slice->ssize.length);
        }

        if (count < 0) {
            break;
        }

        node = UNIQ_SKIPLIST_LEVEL0_PREV_NODE(node);
    }
    }
}
*/

static int get_slices(OBSharedContext *ctx, OBEntry *ob,
        const FSBlockSliceKeyInfo *bs_key, OBSlicePtrArray *sarray)
{
    UniqSkiplistNode *node;
    UniqSkiplistNode *previous;
    OBSliceEntry target;
    OBSliceEntry *curr_slice;
    int slice_end;
    int curr_end;
    int length;
    int result;

    //print_skiplist(ob);

    target.ssize = bs_key->slice;

    /*
    logInfo("target slice.offset: %d, length: %d",
            target.ssize.offset, target.ssize.length);
            */

    node = uniq_skiplist_find_ge_node(ob->slices, &target);
    if (node == NULL) {
        previous = UNIQ_SKIPLIST_LEVEL0_TAIL_NODE(ob->slices);
    } else {
        previous = UNIQ_SKIPLIST_LEVEL0_PREV_NODE(node);
    }

    slice_end = bs_key->slice.offset + bs_key->slice.length;

    /*
    logInfo("bs_key->slice.offset: %d, length: %d, slice_end: %d, ge node: %p, top: %p",
            bs_key->slice.offset, bs_key->slice.length, slice_end, node, ob->slices->top);
            */

    if (previous != ob->slices->top) {
        curr_slice = (OBSliceEntry *)previous->data;
        curr_end = curr_slice->ssize.offset + curr_slice->ssize.length;

        /*
        logInfo("previous slice.offset: %d, length: %d, curr_end: %d",
                curr_slice->ssize.offset, curr_slice->ssize.length, curr_end);
                */

        if (curr_end > bs_key->slice.offset) {  //overlap
            length = FC_MIN(curr_end, slice_end) - bs_key->slice.offset;
            if ((result=dup_slice_to_array(ctx, curr_slice, bs_key->
                            slice.offset, length, sarray)) != 0)
            {
                return result;
            }
        }
    }

    if (node == NULL) {
        return sarray->count > 0 ? 0 : ENOENT;
    }

    result = 0;
    do {
        curr_slice = (OBSliceEntry *)node->data;
        if (slice_end <= curr_slice->ssize.offset) {  //not overlap
            break;
        }

        curr_end = curr_slice->ssize.offset + curr_slice->ssize.length;

        /*
        logInfo("current slice.offset: %d, length: %d, curr_end: %d",
                curr_slice->ssize.offset, curr_slice->ssize.length, curr_end);
                */

        if (curr_end > slice_end) {  //the last slice
            if ((result=dup_slice_to_array(ctx, curr_slice, curr_slice->
                            ssize.offset, slice_end - curr_slice->
                            ssize.offset, sarray)) != 0)
            {
                return result;
            }
        } else {
            __sync_add_and_fetch(&curr_slice->ref_count, 1);
            if ((result=add_to_slice_ptr_array(sarray, curr_slice)) != 0) {
                return result;
            }
        }

        node = UNIQ_SKIPLIST_LEVEL0_NEXT_NODE(node);
    } while (node != ob->slices->factory->tail);

    return sarray->count > 0 ? 0 : ENOENT;
}

static void free_slices(OBSlicePtrArray *sarray)
{
    OBSliceEntry **pp;
    OBSliceEntry **end;

    if (sarray->count == 0) {
        return;
    }

    end = sarray->slices + sarray->count;
    for (pp=sarray->slices; pp<end; pp++) {
        ob_index_free_slice(*pp);
    }

    sarray->count = 0;
}

int ob_index_get_slices(const FSBlockSliceKeyInfo *bs_key,
        OBSlicePtrArray *sarray)
{
    OBSharedContext *ctx;
    OBEntry **bucket;
    OBEntry *ob;
    int64_t bucket_index;
    int result;

    sarray->count = 0;
    bucket_index = FS_BLOCK_HASH_CODE(bs_key->block) %
        ob_hashtable.capacity;
    ctx = ob_shared_ctx_array.contexts + bucket_index %
        ob_shared_ctx_array.count;
    bucket = ob_hashtable.buckets + bucket_index;

    pthread_mutex_lock(&ctx->lock);
    ob = get_ob_entry(ctx, bucket, &bs_key->block, false);
    if (ob == NULL) {
        result = ENOENT;
    } else {
        result = get_slices(ctx, ob, bs_key, sarray);
    }
    pthread_mutex_unlock(&ctx->lock);

    if (result != 0 && sarray->count > 0) {
        free_slices(sarray);
    }
    return result;
}

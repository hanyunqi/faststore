//binlog_writer.h

#ifndef _BINLOG_WRITER_H_
#define _BINLOG_WRITER_H_

#include "fastcommon/fc_queue.h"
#include "binlog_types.h"

#define FS_BINLOG_WRITER_TYPE_ORDER_BY_NONE    0
#define FS_BINLOG_WRITER_TYPE_ORDER_BY_VERSION 1

struct binlog_writer_info;

typedef struct binlog_writer_ptr_array {
    struct binlog_writer_info **entries;
    int count;
    int alloc;
} BinlogWriterPtrArray;

typedef struct binlog_writer_buffer {
    int64_t version;
    BufferInfo bf;
    struct binlog_writer_info *writer;
    struct binlog_writer_buffer *next;
} BinlogWriterBuffer;

typedef struct binlog_writer_buffer_ring {
    BinlogWriterBuffer **entries;
    BinlogWriterBuffer **start; //for consumer
    BinlogWriterBuffer **end;   //for producer
    int count;
    int max_count;
    int size;
} BinlogWriterBufferRing;

typedef struct binlog_writer_thread {
    struct fast_mblock_man mblock;
    struct fc_queue queue;
    volatile bool running;
    int order_by;
    BinlogWriterPtrArray flush_writers;
} BinlogWriterThread;

typedef struct binlog_writer_info {
    struct {
        char subdir_name[FS_BINLOG_SUBDIR_NAME_SIZE];
        int max_record_size;
    } cfg;

    struct {
        int index;
        int compress_index;
    } binlog;

    struct {
        int fd;
        int64_t size;
        char *name;
    } file;

    struct {
        BinlogWriterBufferRing ring;
        int64_t next;
    } version_ctx;
    ServerBinlogBuffer binlog_buffer;
    BinlogWriterThread *thread;
} BinlogWriterInfo;

typedef struct binlog_writer_context {
    BinlogWriterInfo writer;
    BinlogWriterThread thread;
} BinlogWriterContext;

#ifdef __cplusplus
extern "C" {
#endif

int binlog_writer_init_normal(BinlogWriterInfo *writer,
        const char *subdir_name);

int binlog_writer_init_by_version(BinlogWriterInfo *writer,
        const char *subdir_name, const uint64_t next_version,
        const int ring_size);

int binlog_writer_init_thread_ex(BinlogWriterThread *thread,
        BinlogWriterInfo *writer, const int order_by,
        const int max_record_size, const int writer_count);

#define binlog_writer_init_thread(thread, writer, order_by, max_record_size) \
    binlog_writer_init_thread_ex(thread, writer, order_by, max_record_size, 1)

static inline int binlog_writer_init(BinlogWriterContext *context,
        const char *subdir_name, const int max_record_size)
{
    int result;
    if ((result=binlog_writer_init_normal(&context->writer,
                    subdir_name)) != 0)
    {
        return result;
    }

    return binlog_writer_init_thread(&context->thread, &context->writer,
            FS_BINLOG_WRITER_TYPE_ORDER_BY_NONE, max_record_size);
}

static inline void binlog_writer_set_next_version(BinlogWriterInfo *writer,
        const uint64_t next_version)
{
    writer->version_ctx.next = next_version;
    writer->version_ctx.ring.start = writer->version_ctx.ring.end =
        writer->version_ctx.ring.entries + next_version %
        writer->version_ctx.ring.size;
}

void binlog_writer_finish(BinlogWriterInfo *writer);

int binlog_get_current_write_index(BinlogWriterInfo *writer);

void binlog_get_current_write_position(BinlogWriterInfo *writer,
        FSBinlogFilePosition *position);

static inline BinlogWriterBuffer *binlog_writer_alloc_buffer(
        BinlogWriterThread *thread)
{
    return (BinlogWriterBuffer *)fast_mblock_alloc_object(&thread->mblock);
}


static inline const char *binlog_writer_get_filename(BinlogWriterInfo *writer,
        const int binlog_index, char *filename, const int size)
{
    snprintf(filename, size, "%s/%s/%s"BINLOG_FILE_EXT_FMT,
            DATA_PATH_STR, writer->cfg.subdir_name,
            BINLOG_FILE_PREFIX, binlog_index);
    return filename;
}

#define push_to_binlog_write_queue(thread, buffer) \
    fc_queue_push(&(thread)->queue, buffer)

#ifdef __cplusplus
}
#endif

#endif

//binlog_types.h

#ifndef _BINLOG_TYPES_H_
#define _BINLOG_TYPES_H_

#include <time.h>
#include <limits.h>
#include <pthread.h>
#include "fastcommon/fast_buffer.h"
#include "fastcommon/common_blocked_queue.h"
#include "../server_types.h"

#define BINLOG_FILE_PREFIX     "binlog"
#define BINLOG_FILE_EXT_FMT    ".%06d"

#define BINLOG_BUFFER_INIT_SIZE      4096
#define BINLOG_BUFFER_LENGTH(buffer) ((buffer).end - (buffer).buff)
#define BINLOG_BUFFER_REMAIN(buffer) ((buffer).end - (buffer).current)

struct fs_binlog_record;

typedef void (*data_thread_notify_func)(struct fs_binlog_record *record,
        const int result, const bool is_error);

typedef struct server_binlog_buffer {
    char *buff;    //the buffer pointer
    char *current; //for the consumer
    char *end;     //data end ptr
    int size;      //the buffer size (capacity)
} ServerBinlogBuffer;

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif

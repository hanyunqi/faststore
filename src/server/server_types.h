#ifndef _FS_SERVER_TYPES_H
#define _FS_SERVER_TYPES_H

#include <time.h>
#include <pthread.h>
#include "fastcommon/common_define.h"
#include "fastcommon/fc_queue.h"
#include "fastcommon/fast_task_queue.h"
#include "fastcommon/fast_mblock.h"
#include "fastcommon/fast_allocator.h"
#include "fastcommon/server_id_func.h"
#include "common/fs_types.h"
#include "storage/storage_types.h"

#define FS_TRUNK_BINLOG_MAX_RECORD_SIZE    128
#define FS_TRUNK_BINLOG_SUBDIR_NAME      "trunk"

#define FS_SLICE_BINLOG_MAX_RECORD_SIZE    256
#define FS_SLICE_BINLOG_SUBDIR_NAME      "slice"

#define FS_REPLICA_BINLOG_MAX_RECORD_SIZE  128
#define FS_REPLICA_BINLOG_SUBDIR_NAME    "replica"

#define FS_CLUSTER_TASK_TYPE_NONE               0
#define FS_CLUSTER_TASK_TYPE_RELATIONSHIP       1   //slave  -> master
#define FS_CLUSTER_TASK_TYPE_REPLICATION        2

#define FS_REPLICATION_STAGE_NONE               0
#define FS_REPLICATION_STAGE_INITED             1
#define FS_REPLICATION_STAGE_CONNECTING         2
#define FS_REPLICATION_STAGE_WAITING_JOIN_RESP  3
#define FS_REPLICATION_STAGE_SYNCING            4

#define FS_DEFAULT_REPLICA_CHANNELS_BETWEEN_TWO_SERVERS  2
#define FS_DEFAULT_TRUNK_FILE_SIZE  (  1 * 1024 * 1024 * 1024LL)
#define FS_TRUNK_FILE_MIN_SIZE      (256 * 1024 * 1024LL)
#define FS_TRUNK_FILE_MAX_SIZE      ( 16 * 1024 * 1024 * 1024LL)

#define FS_DEFAULT_DISCARD_REMAIN_SPACE_SIZE  4096
#define FS_DISCARD_REMAIN_SPACE_MIN_SIZE       256
#define FS_DISCARD_REMAIN_SPACE_MAX_SIZE      (256 * 1024)

#define TASK_STATUS_CONTINUE   12345

#define TASK_ARG          ((FSServerTaskArg *)task->arg)
#define TASK_CTX          TASK_ARG->context
#define REQUEST           TASK_CTX.request
#define RESPONSE          TASK_CTX.response
#define RESPONSE_STATUS   RESPONSE.header.status
#define REQUEST_STATUS    REQUEST.header.status
#define RECORD            TASK_CTX.service.record
#define WAITING_RPC_COUNT TASK_CTX.service.waiting_rpc_count
#define CLUSTER_PEER      TASK_CTX.cluster.peer
#define CLUSTER_REPLICA   TASK_CTX.cluster.replica
#define CLUSTER_CONSUMER_CTX  TASK_CTX.cluster.consumer_ctx
#define CLUSTER_TASK_TYPE TASK_CTX.cluster.task_type

typedef void (*server_free_func)(void *ptr);
typedef void (*server_free_func_ex)(void *ctx, void *ptr);

typedef struct {
    int index;   //the inner index is important!
    string_t path;
} FSStorePath;

typedef struct {
    int64_t id;
    int64_t subdir;     //in which subdir
} FSTrunkIdInfo;

typedef struct {
    FSStorePath *store;
    FSTrunkIdInfo id_info;
    int64_t offset; //offset of the trunk file
    int64_t size;   //alloced space size
} FSTrunkSpaceInfo;

typedef struct fs_binlog_file_position {
    int index;      //current binlog file
    int64_t offset; //current file offset
} FSBinlogFilePosition;

struct fs_replication;
typedef struct fs_replication_array {
    struct fs_replication *replications;
    int count;
} FSReplicationArray;

typedef struct fs_replication_ptr_array {
    int count;
    struct fs_replication **replications;
} FSReplicationPtrArray;

typedef struct fs_data_server_change_event {
    int data_group_index;
    int data_server_index;
    bool in_queue;
    struct fs_data_server_change_event *next;  //for queue
} FSDataServerChangeEvent;

typedef struct fs_cluster_topology_notify_context {
    pthread_mutex_t lock;  //for lock FSDataServerChangeEvent
    struct fc_queue queue; //push data_server changes to the follower
    struct fs_data_server_change_event *events; //event array
} FSClusterTopologyNotifyContext;

typedef struct fs_cluster_server_info {
    FCServerInfo *server;
    FSReplicationPtrArray repl_ptr_array;
    FSClusterTopologyNotifyContext notify_ctx;
    bool is_leader;
    int server_index;
    int link_index;      //for next links
} FSClusterServerInfo;

typedef struct fs_cluster_server_array {
    FSClusterServerInfo *servers;
    int count;
} FSClusterServerArray;

typedef struct fs_cluster_server_pp_array {
    FSClusterServerInfo **servers;
    int count;
} FSClusterServerPPArray;

typedef struct fs_cluster_data_server_info {
    FSClusterServerInfo *cs;
    bool is_master;
    char status;                 //the data server status
    int index;
    int64_t last_data_version;   //for replication
    int64_t last_report_version; //for report last data version to the leader
    //int64_t change_version;    //for notify to the follower
} FSClusterDataServerInfo;

typedef struct fs_cluster_data_server_array {
    FSClusterDataServerInfo *servers;
    int count;
} FSClusterDataServerArray;

typedef struct fs_cluster_data_group_info {
    int data_group_id;
    bool include_myself;
    FSClusterDataServerArray data_server_array;
    FSClusterServerPPArray active_slaves;
} FSClusterDataGroupInfo;

typedef struct fs_cluster_data_group_array {
    FSClusterDataGroupInfo *groups;
    int count;
    int base_id;
} FSClusterDataGroupArray;

typedef struct fdir_binlog_push_result_entry {
    uint64_t data_version;
    int64_t task_version;
    time_t expires;
    struct fast_task_info *waiting_task;
    struct fdir_binlog_push_result_entry *next;
} FSBinlogPushResultEntry;

typedef struct fdir_binlog_push_result_context {
    struct {
        FSBinlogPushResultEntry *entries;
        FSBinlogPushResultEntry *start; //for consumer
        FSBinlogPushResultEntry *end;   //for producer
        int size;
    } ring;

    struct {
        FSBinlogPushResultEntry *head;
        FSBinlogPushResultEntry *tail;
        struct fast_mblock_man rentry_allocator;
    } queue;   //for overflow exceptions

    time_t last_check_timeout_time;
} FSBinlogPushResultContext;

typedef struct fs_replication_context {
    struct fc_queue queue;  //push to peer
    FSBinlogPushResultContext push_result_ctx;   //push result recv from peer
} FSReplicationContext;

typedef struct fs_replication {
    struct fast_task_info *task;
    FSClusterServerInfo *peer;
    short stage;
    bool is_client;
    int thread_index; //for nio thread
    int conn_index;
    struct {
        int start_time;
        int next_connect_time;
        int last_errno;
        int fail_count;
        ConnectionInfo conn;
    } connection_info;

    FSReplicationContext context;
} FSReplication;

typedef struct {
    FSRequestInfo request;
    FSResponseInfo response;
    int (*deal_func)(struct fast_task_info *task);
    bool response_done;
    bool log_error;
    bool need_response;

    union {
        struct {
            volatile int waiting_rpc_count;
        } service;

        struct {
            int task_type;

            FSClusterServerInfo *peer;   //the peer server in the cluster

            FSReplication *replica;
        } cluster;
    };

    FSBlockSliceKeyInfo bs_key;
    FSSliceOpNotify slice_notify;
} FSServerTaskContext;

typedef struct server_task_arg {
    volatile int64_t task_version;
    int64_t req_start_time;

    FSServerTaskContext context;
} FSServerTaskArg;


struct ob_slice_ptr_array;
typedef struct fs_server_context {
    union {
        struct {
            struct fast_mblock_man record_allocator;
            struct ob_slice_ptr_array *slice_ptr_array;
        } service;

        struct {
            FSReplicationPtrArray connectings;  //master side
            FSReplicationPtrArray connected;    //master side
        } cluster;
    };

} FSServerContext;

#endif

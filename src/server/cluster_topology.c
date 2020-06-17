#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include "fastcommon/logger.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "sf/sf_global.h"
#include "sf/sf_nio.h"
#include "common/fs_proto.h"
#include "server_global.h"
#include "cluster_topology.h"

static FSClusterDataServerInfo *find_data_group_server(
        const int gindex, FSClusterServerInfo *cs)
{
    FSClusterDataServerArray *ds_array;
    FSClusterDataServerInfo *ds;
    FSClusterDataServerInfo *end;

    ds_array = &CLUSTER_DATA_RGOUP_ARRAY.groups[gindex].data_server_array;
    end = ds_array->servers + ds_array->count;
    for (ds=ds_array->servers; ds<end; ds++) {
        if (ds->cs == cs) {
            logInfo("data group index: %d, server id: %d", gindex, cs->server->id);
            return ds;
        }
    }

    return NULL;
}

int cluster_topology_init_notify_ctx(FSClusterTopologyNotifyContext *notify_ctx)
{
    int result;
    int count;
    int bytes;
    int index;
    int i;
    FSClusterServerInfo *cs;
    FSClusterServerInfo *end;

    if ((result=fc_queue_init(&notify_ctx->queue, (long)
                    (&((FSDataServerChangeEvent *)NULL)->next))) != 0)
    {
        return result;
    }

    count = CLUSTER_DATA_RGOUP_ARRAY.count * CLUSTER_SERVER_ARRAY.count;
    bytes = sizeof(FSDataServerChangeEvent) * count;
    notify_ctx->events = (FSDataServerChangeEvent *)malloc(bytes);
    if (notify_ctx->events == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail", __LINE__, bytes);
        return ENOMEM;
    }
    memset(notify_ctx->events, 0, bytes);

    logInfo("data group count: %d, server count: %d\n",
            CLUSTER_DATA_RGOUP_ARRAY.count, CLUSTER_SERVER_ARRAY.count);

    end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (i=0; i<CLUSTER_DATA_RGOUP_ARRAY.count; i++) {
        for (cs=CLUSTER_SERVER_ARRAY.servers; cs<end; cs++) {
            index = i * CLUSTER_SERVER_ARRAY.count + cs->server_index;
            notify_ctx->events[index].data_server =
                find_data_group_server(i, cs);
        }
    }

    return 0;
}

int cluster_topology_data_server_chg_notify(FSClusterDataServerInfo *
        data_server, const bool notify_self)
{
    FSClusterServerInfo *cs;
    FSClusterServerInfo *end;
    FSDataServerChangeEvent *event;
    struct fast_task_info *task;
    bool notify;

    end = CLUSTER_SERVER_ARRAY.servers + CLUSTER_SERVER_ARRAY.count;
    for (cs=CLUSTER_SERVER_ARRAY.servers; cs<end; cs++) {
        if (cs->is_leader || (!notify_self && data_server->cs == cs) ||
                (__sync_fetch_and_add(&cs->active, 0) == 0))
        {
            continue;
        }
        task = (struct fast_task_info *)cs->notify_ctx.task;
        if (task == NULL) {
            continue;
        }

        event = cs->notify_ctx.events + (data_server->dg->index *
                CLUSTER_SERVER_ARRAY.count + data_server->cs->server_index);
        assert(event->data_server == data_server);

        if (__sync_bool_compare_and_swap(&event->in_queue, 0, 1)) { //fetch event
            fc_queue_push_ex(&cs->notify_ctx.queue, event, &notify);
            if (notify) {
                iovent_notify_thread(task->thread_data);
            }
        }
    }

    return 0;
}

static int process_notify_events(FSClusterTopologyNotifyContext *ctx)
{
    FSDataServerChangeEvent *event;
    FSProtoHeader *header;
    FSProtoPushDataServerStatusReqHeader *req_header;
    FSProtoPushDataServerStatusReqBodyPart *bp_start;
    FSProtoPushDataServerStatusReqBodyPart *body_part;
    int body_len;

    if (!(ctx->task->offset == 0 && ctx->task->length == 0)) {
        return EBUSY;
    }

    event = (FSDataServerChangeEvent *)fc_queue_try_pop_all(&ctx->queue);
    if (event == NULL) {
        return 0;
    }

    header = (FSProtoHeader *)ctx->task->data;
    req_header = (FSProtoPushDataServerStatusReqHeader *)(header + 1);
    bp_start = (FSProtoPushDataServerStatusReqBodyPart *)(req_header + 1);
    body_part = bp_start;
    while (event != NULL) {
        int2buff(event->data_server->dg->id, body_part->data_group_id);
        int2buff(event->data_server->cs->server->id, body_part->server_id);
        body_part->is_master = event->data_server->is_master;
        body_part->status = event->data_server->status;
        int2buff(event->data_server->data_version, body_part->data_version);

        __sync_bool_compare_and_swap(&event->in_queue, 1, 0);  //release event

        ++body_part;
        event = event->next;
    }

    int2buff(body_part - bp_start, req_header->data_server_count);
    body_len = (char *)body_part - (char *)req_header;
    FS_PROTO_SET_HEADER(header, FS_CLUSTER_PROTO_PUSH_DATA_SERVER_STATUS,
            body_len);
    ctx->task->length = sizeof(FSProtoHeader) + body_len;
    return sf_send_add_event((struct fast_task_info *)ctx->task);
}

int cluster_topology_process_notify_events(FSClusterNotifyContextPtrArray *
        notify_ctx_ptr_array)
{
    FSClusterTopologyNotifyContext **ctx;
    FSClusterTopologyNotifyContext **end;

    end = notify_ctx_ptr_array->contexts + notify_ctx_ptr_array->count;
    for (ctx=notify_ctx_ptr_array->contexts; ctx<end; ctx++) {
        process_notify_events(*ctx);
    }

    return 0;
}
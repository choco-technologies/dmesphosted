#include "private.h"
#include <errno.h>
#include <string.h>

/* Room for the request's frames in the queue: then the bus takes them */
#define RPC_SEND_TIMEOUT_MS     1000

int rpc_call(dmdrvi_context_t context, uint32_t msg_id, rpc_build_t build, const void *build_arg,
             rpc_read_t read, void *read_arg, uint32_t timeout_ms)
{
    dmosi_mutex_lock(context->rpc_lock);

    dmosi_mutex_lock(context->lock);
    bool up = context->bus_state == BUS_UP;
    dmosi_mutex_unlock(context->lock);
    if (!up)
    {
        dmosi_mutex_unlock(context->rpc_lock);
        return -ENODEV;
    }

    pb_writer_t w;
    pb_writer_init(&w, context->rpc_buf, RPC_MAX_MESSAGE);
    rpc_begin_request(&w, msg_id, ++context->rpc_uid);
    if (build != NULL)
        build(&w, build_arg);
    size_t n = rpc_end_request(&w);
    size_t m = (n != 0) ? rpc_wrap(context->rpc_out, RPC_MAX_MESSAGE, RPC_EP_REQUEST, context->rpc_buf, n) : 0;
    if (m == 0)
    {
        dmosi_mutex_unlock(context->rpc_lock);
        return -EMSGSIZE;
    }

    /* A late answer of an earlier call that gave up must not count */
    (void)dmosi_semaphore_wait(context->rpc_done, 1, 0);
    dmosi_mutex_lock(context->lock);
    context->rpc_wait_id = msg_id + RPC_RESP_OFFSET;
    dmosi_mutex_unlock(context->lock);

    int ret = bus_send_serial(context, context->rpc_out, m, RPC_SEND_TIMEOUT_MS);
    if (ret == 0 && dmosi_semaphore_wait(context->rpc_done, 1, (int32_t)timeout_ms) != 0)
        ret = -ETIMEDOUT;

    dmosi_mutex_lock(context->lock);
    context->rpc_wait_id = 0;
    dmosi_mutex_unlock(context->lock);

    if (ret == 0)
    {
        rpc_message_t resp;
        int32_t       code;
        if (!rpc_decode(context->rpc_buf, context->rpc_len, &resp))
            ret = -EIO;
        else if ((code = rpc_resp_code(&resp)) != 0)
        {
            DMOD_LOG_WARN("%s: RPC 0x%03x: the co-processor answered esp_err_t 0x%x\n",
                          context->iface_name, (unsigned)msg_id, (unsigned)code);
            ret = -EIO;
        }
        else if (read != NULL)
            ret = read(&resp, read_arg);
    }
    else
    {
        DMOD_LOG_WARN("%s: RPC 0x%03x: no answer (%d)\n", context->iface_name, (unsigned)msg_id, ret);
    }

    dmosi_mutex_unlock(context->rpc_lock);
    return ret;
}

void rpc_received(dmdrvi_context_t context, const uint8_t *data, size_t len)
{
    char           endpoint[RPC_EP_NAME_MAX];
    const uint8_t *pb;
    size_t         pb_len;
    rpc_message_t  msg;

    if (!rpc_unwrap(data, len, endpoint, &pb, &pb_len) || !rpc_decode(pb, pb_len, &msg))
    {
        DMOD_LOG_WARN("%s: an RPC message not understood (%u bytes)\n", context->iface_name, (unsigned)len);
        return;
    }

    if (msg.type == RPC_TYPE_RESP)
    {
        dmosi_mutex_lock(context->lock);
        bool waited = context->rpc_wait_id == msg.msg_id && pb_len <= RPC_MAX_MESSAGE;
        if (waited)
        {
            memcpy(context->rpc_buf, pb, pb_len);
            context->rpc_len     = pb_len;
            context->rpc_wait_id = 0;
        }
        dmosi_mutex_unlock(context->lock);
        if (waited)
            (void)dmosi_semaphore_post(context->rpc_done, 1);
    }
    else if (msg.type == RPC_TYPE_EVENT)
    {
        wifi_event(context, &msg);
    }
}

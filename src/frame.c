#include "frame.h"
#include <string.h>

#define OFFSET_LEN          2u
#define OFFSET_OFFSET       4u
#define OFFSET_CHECKSUM     6u
#define OFFSET_SEQ          8u
#define OFFSET_THROTTLE     10u
#define OFFSET_PRIV_TYPE    11u

/* The co-processor accepts a little padding before the payload (legacy hosts) */
#define MAX_PAYLOAD_OFFSET  (FRAME_HEADER_SIZE + 7u)

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint16_t frame_checksum(const uint8_t *buf, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint16_t)(sum + buf[i]);
    return sum;
}

size_t frame_encode(uint8_t *buf, const frame_t *frame, bool checksum)
{
    uint16_t len = (frame != NULL && frame->payload != NULL) ? frame->len : 0;
    if (len > FRAME_MAX_PAYLOAD)
        return 0;

    /* The payload may already be in place (buf + FRAME_HEADER_SIZE) */
    if (len != 0)
        memmove(buf + FRAME_HEADER_SIZE, frame->payload, len);
    memset(buf, 0, FRAME_HEADER_SIZE);
    memset(buf + FRAME_HEADER_SIZE + len, 0, FRAME_BUFFER_SIZE - FRAME_HEADER_SIZE - len);
    if (len == 0)
    {
        /* Nothing: what the co-processor itself sends then (eh_frame_encode_dummy) */
        buf[0] = FRAME_IF_DUMMY;
        put_le16(buf + OFFSET_OFFSET, FRAME_HEADER_SIZE);
        return FRAME_HEADER_SIZE;
    }

    buf[0] = (uint8_t)((frame->if_type & 0x0Fu) | ((frame->if_num & 0x0Fu) << 4));
    buf[1] = frame->flags;
    put_le16(buf + OFFSET_LEN, len);
    put_le16(buf + OFFSET_OFFSET, FRAME_HEADER_SIZE);
    put_le16(buf + OFFSET_SEQ, frame->seq_num);
    buf[OFFSET_THROTTLE] = (uint8_t)(frame->throttle & 0x03u);
    buf[OFFSET_PRIV_TYPE] = frame->priv_pkt_type;

    size_t total = FRAME_HEADER_SIZE + len;
    if (checksum)
        put_le16(buf + OFFSET_CHECKSUM, frame_checksum(buf, total));
    return total;
}

frame_result_t frame_decode(const uint8_t *buf, size_t size, bool checksum, frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    if (size < FRAME_HEADER_SIZE)
        return FRAME_BAD_HEADER;

    frame->if_type       = buf[0] & 0x0Fu;
    frame->if_num        = (uint8_t)(buf[0] >> 4);
    frame->flags         = buf[1];
    frame->seq_num       = get_le16(buf + OFFSET_SEQ);
    frame->throttle      = buf[OFFSET_THROTTLE] & 0x03u;
    frame->priv_pkt_type = buf[OFFSET_PRIV_TYPE];

    uint16_t len    = get_le16(buf + OFFSET_LEN);
    uint16_t offset = get_le16(buf + OFFSET_OFFSET);
    if (frame->if_type == FRAME_IF_DUMMY || len == 0)
        return FRAME_EMPTY;
    if (offset < FRAME_HEADER_SIZE || offset > MAX_PAYLOAD_OFFSET || (size_t)offset + len > size)
        return FRAME_BAD_HEADER;

    if (checksum)
    {
        uint16_t stored = get_le16(buf + OFFSET_CHECKSUM);
        uint16_t sum    = (uint16_t)(frame_checksum(buf, (size_t)offset + len) -
                                     buf[OFFSET_CHECKSUM] - buf[OFFSET_CHECKSUM + 1]);
        if (stored != sum)
            return FRAME_BAD_CHECKSUM;
    }

    frame->payload = buf + offset;
    frame->len     = len;
    return FRAME_OK;
}

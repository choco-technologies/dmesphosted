#ifndef DMESPHOSTED_FRAME_H
#define DMESPHOSTED_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The frames ESP-Hosted's co-processor and its host exchange - one per SPI
 * transaction, always FRAME_BUFFER_SIZE bytes clocked both ways. Every frame
 * starts with the 12-byte header (V1, little endian):
 *
 *   0   if_type:4 | if_num:4      what the payload is (FRAME_IF_*)
 *   1   flags                     FRAME_FLAG_MORE_FRAGMENT
 *   2   len (2)                   the payload's bytes
 *   4   offset (2)                where the payload starts (= 12)
 *   6   checksum (2)              byte sum of header and payload (field = 0)
 *   8   seq_num (2)
 *   10  throttle_cmd:2 | 6        V1 flow control (FRAME_THROTTLE_ON)
 *   11  priv_pkt_type             of a FRAME_IF_PRIV frame (FRAME_PRIV_*)
 *
 * A frame with nothing in it (len 0, or if_type FRAME_IF_DUMMY) is what a
 * side clocks out when it has nothing to say.
 */

#define FRAME_BUFFER_SIZE           1600u   /* ESP_TRANSPORT_SPI_MAX_BUF_SIZE */
#define FRAME_HEADER_SIZE           12u
#define FRAME_MAX_PAYLOAD           (FRAME_BUFFER_SIZE - FRAME_HEADER_SIZE)

/* if_type (eh_if_type_t: the wire-stable numbering, not the legacy one) */
#define FRAME_IF_STA                1u      /* Station's 802.3 frames */
#define FRAME_IF_AP                 2u      /* SoftAP's 802.3 frames */
#define FRAME_IF_SERIAL             3u      /* RPC (protocomm TLV + protobuf), fragmented */
#define FRAME_IF_HCI                4u
#define FRAME_IF_PRIV               5u      /* Bring-up: capabilities, host config */
#define FRAME_IF_TEST               6u
#define FRAME_IF_DUMMY              8u      /* ESP_MAX_IF: nothing */

#define FRAME_FLAG_MORE_FRAGMENT    0x01u

/* throttle_cmd of a co-processor's frame: 1 while its queue is too full for
 * 802.3 frames (only when the host gave it thresholds - this one does not) */
#define FRAME_THROTTLE_ON           1u

/* priv_pkt_type */
#define FRAME_PRIV_EVENT            0x33u   /* ESP_PACKET_TYPE_EVENT */

typedef struct
{
    uint8_t         if_type;
    uint8_t         if_num;
    uint8_t         flags;
    uint8_t         throttle;
    uint8_t         priv_pkt_type;
    uint16_t        seq_num;
    const uint8_t  *payload;    /* Into the decoded buffer */
    uint16_t        len;
} frame_t;

typedef enum
{
    FRAME_OK = 0,
    FRAME_EMPTY,                /* Nothing in it */
    FRAME_BAD_HEADER,           /* Offset or length out of the buffer */
    FRAME_BAD_CHECKSUM,
} frame_result_t;

/* Byte sum, 16 bits, wrapping (eh_frame_checksum()) */
uint16_t frame_checksum(const uint8_t *buf, size_t len);

/**
 * Writes a frame into @p buf (FRAME_BUFFER_SIZE bytes: the rest is zeroed).
 * The payload may already be in place, at buf + FRAME_HEADER_SIZE. A NULL
 * @p frame, or one with no payload, writes an empty frame.
 * Returns the bytes used (header + payload), 0 when the payload does not fit.
 */
size_t frame_encode(uint8_t *buf, const frame_t *frame, bool checksum);

/**
 * Reads the frame of a received buffer of @p size bytes; @p frame->payload
 * points into @p buf. The checksum is checked when @p checksum is set.
 */
frame_result_t frame_decode(const uint8_t *buf, size_t size, bool checksum, frame_t *frame);

#endif /* DMESPHOSTED_FRAME_H */

#ifndef DMESPHOSTED_PB_H
#define DMESPHOSTED_PB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Just as much of the protobuf wire format as the RPC needs: varints,
 * length-delimited fields and messages in messages. Proto3 - a field at its
 * default (0, false, empty) is not written; the reader skips what it does
 * not know.
 */

#define PB_WIRE_VARINT      0u
#define PB_WIRE_FIXED64     1u
#define PB_WIRE_LEN         2u
#define PB_WIRE_FIXED32     5u

#define PB_MAX_DEPTH        4u

typedef struct
{
    uint8_t    *buf;
    size_t      size;
    size_t      pos;
    size_t      open[PB_MAX_DEPTH];   /* Where each open message's length goes */
    unsigned    depth;
    bool        overflow;
} pb_writer_t;

void pb_writer_init(pb_writer_t *w, uint8_t *buf, size_t size);

/* An integer field (int32 is sign-extended to 64 bits, as protobuf does) */
void pb_put_int(pb_writer_t *w, uint32_t field, int64_t value);
void pb_put_uint(pb_writer_t *w, uint32_t field, uint64_t value);
void pb_put_bool(pb_writer_t *w, uint32_t field, bool value);
void pb_put_bytes(pb_writer_t *w, uint32_t field, const void *data, size_t len);

/*
 * A message in the message: what is put between them is its content. Written
 * even when empty - a oneof's message is there because it is set.
 */
void pb_begin(pb_writer_t *w, uint32_t field);
void pb_end(pb_writer_t *w);

/* The bytes written, 0 when they did not fit (or a message was left open) */
size_t pb_writer_done(const pb_writer_t *w);

typedef struct
{
    const uint8_t  *pos;
    const uint8_t  *end;
} pb_reader_t;

typedef struct
{
    uint32_t        number;
    uint8_t         wire;
    uint64_t        value;      /* PB_WIRE_VARINT, FIXED32/64 */
    const uint8_t  *data;       /* PB_WIRE_LEN */
    size_t          len;
} pb_field_t;

void pb_reader_init(pb_reader_t *r, const uint8_t *data, size_t len);

/* The next field; false at the end, or when the data is broken */
bool pb_next(pb_reader_t *r, pb_field_t *field);

/* A length-delimited field's content, as a message */
void pb_reader_sub(pb_reader_t *r, const pb_field_t *field);

/* The value of a varint field as protobuf's int32 */
static inline int32_t pb_int32(const pb_field_t *field)
{
    return (int32_t)(uint32_t)field->value;
}

#endif /* DMESPHOSTED_PB_H */

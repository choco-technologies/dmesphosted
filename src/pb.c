#include "pb.h"
#include <string.h>

/* A message's length is reserved as one byte and moved apart when longer */
#define MAX_VARINT_LEN  10u

static size_t varint_len(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80u)
    {
        v >>= 7;
        n++;
    }
    return n;
}

static void put_raw_varint(pb_writer_t *w, uint64_t v)
{
    if (w->overflow || w->pos + varint_len(v) > w->size)
    {
        w->overflow = true;
        return;
    }
    while (v >= 0x80u)
    {
        w->buf[w->pos++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    w->buf[w->pos++] = (uint8_t)v;
}

static void put_tag(pb_writer_t *w, uint32_t field, uint8_t wire)
{
    put_raw_varint(w, ((uint64_t)field << 3) | wire);
}

void pb_writer_init(pb_writer_t *w, uint8_t *buf, size_t size)
{
    memset(w, 0, sizeof(*w));
    w->buf  = buf;
    w->size = size;
}

void pb_put_uint(pb_writer_t *w, uint32_t field, uint64_t value)
{
    if (value == 0)
        return;
    put_tag(w, field, PB_WIRE_VARINT);
    put_raw_varint(w, value);
}

void pb_put_int(pb_writer_t *w, uint32_t field, int64_t value)
{
    pb_put_uint(w, field, (uint64_t)value);
}

void pb_put_bool(pb_writer_t *w, uint32_t field, bool value)
{
    pb_put_uint(w, field, value ? 1u : 0u);
}

void pb_put_bytes(pb_writer_t *w, uint32_t field, const void *data, size_t len)
{
    if (len == 0)
        return;
    put_tag(w, field, PB_WIRE_LEN);
    put_raw_varint(w, len);
    if (w->overflow || w->pos + len > w->size)
    {
        w->overflow = true;
        return;
    }
    memcpy(w->buf + w->pos, data, len);
    w->pos += len;
}

void pb_begin(pb_writer_t *w, uint32_t field)
{
    if (w->depth >= PB_MAX_DEPTH)
    {
        w->overflow = true;
        return;
    }
    put_tag(w, field, PB_WIRE_LEN);
    w->open[w->depth++] = w->pos;
    if (w->overflow || w->pos + 1 > w->size)
    {
        w->overflow = true;
        return;
    }
    w->pos++;
}

void pb_end(pb_writer_t *w)
{
    if (w->depth == 0)
    {
        w->overflow = true;
        return;
    }
    size_t at = w->open[--w->depth];
    if (w->overflow)
        return;

    size_t   content = w->pos - at - 1;
    size_t   extra   = varint_len(content) - 1;
    if (extra != 0)
    {
        if (w->pos + extra > w->size)
        {
            w->overflow = true;
            return;
        }
        memmove(w->buf + at + 1 + extra, w->buf + at + 1, content);
        w->pos += extra;
    }

    uint64_t v = content;
    size_t   i = at;
    while (v >= 0x80u)
    {
        w->buf[i++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    w->buf[i] = (uint8_t)v;
}

size_t pb_writer_done(const pb_writer_t *w)
{
    return (w->overflow || w->depth != 0) ? 0 : w->pos;
}

void pb_reader_init(pb_reader_t *r, const uint8_t *data, size_t len)
{
    r->pos = data;
    r->end = data + len;
}

static bool get_raw_varint(pb_reader_t *r, uint64_t *v)
{
    uint64_t result = 0;
    for (unsigned shift = 0; shift < 7u * MAX_VARINT_LEN; shift += 7)
    {
        if (r->pos >= r->end)
            return false;
        uint8_t b = *r->pos++;
        result |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0)
        {
            *v = result;
            return true;
        }
    }
    return false;
}

bool pb_next(pb_reader_t *r, pb_field_t *field)
{
    uint64_t tag;
    if (r->pos >= r->end || !get_raw_varint(r, &tag))
        return false;

    memset(field, 0, sizeof(*field));
    field->number = (uint32_t)(tag >> 3);
    field->wire   = (uint8_t)(tag & 0x07u);
    switch (field->wire)
    {
        case PB_WIRE_VARINT:
            return get_raw_varint(r, &field->value);
        case PB_WIRE_FIXED64:
        case PB_WIRE_FIXED32:
        {
            size_t n = (field->wire == PB_WIRE_FIXED64) ? 8u : 4u;
            if ((size_t)(r->end - r->pos) < n)
                return false;
            for (size_t i = 0; i < n; i++)
                field->value |= (uint64_t)r->pos[i] << (8u * i);
            r->pos += n;
            return true;
        }
        case PB_WIRE_LEN:
        {
            uint64_t len;
            if (!get_raw_varint(r, &len) || len > (uint64_t)(r->end - r->pos))
                return false;
            field->data = r->pos;
            field->len  = (size_t)len;
            r->pos += len;
            return true;
        }
        default:
            return false;   /* Groups (3, 4) are not used by proto3 */
    }
}

void pb_reader_sub(pb_reader_t *r, const pb_field_t *field)
{
    pb_reader_init(r, field->data, field->len);
}

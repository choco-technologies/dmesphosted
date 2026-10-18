#include "private.h"
#include <errno.h>
#include <string.h>

/* Slots of a fixed size, allocated once: no allocation per frame */
struct slot
{
    slot_info_t info;
    uint8_t     data[FRAME_MAX_PAYLOAD];
};

int slotq_init(slotq_t *q, uint32_t count)
{
    memset(q, 0, sizeof(*q));
    q->slots = Dmod_Malloc(sizeof(slot_t) * count);
    q->lock  = dmosi_mutex_create(false);
    q->items = dmosi_semaphore_create(0, count);
    q->room  = dmosi_semaphore_create(count, count);
    q->count = count;
    return (q->slots != NULL && q->lock != NULL && q->items != NULL && q->room != NULL) ? 0 : -ENOMEM;
}

void slotq_deinit(slotq_t *q)
{
    if (q->room != NULL)
        dmosi_semaphore_destroy(q->room);
    if (q->items != NULL)
        dmosi_semaphore_destroy(q->items);
    if (q->lock != NULL)
        dmosi_mutex_destroy(q->lock);
    Dmod_Free(q->slots);
    memset(q, 0, sizeof(*q));
}

bool slotq_put(slotq_t *q, const slot_info_t *info, const void *data, int32_t timeout_ms)
{
    if (info->len > FRAME_MAX_PAYLOAD || dmosi_semaphore_wait(q->room, 1, timeout_ms) != 0)
        return false;

    dmosi_mutex_lock(q->lock);
    slot_t *s = &q->slots[(q->head + q->used) % q->count];
    s->info = *info;
    memcpy(s->data, data, info->len);
    q->used++;
    dmosi_mutex_unlock(q->lock);

    dmosi_semaphore_post(q->items, 1);
    return true;
}

bool slotq_get(slotq_t *q, slot_info_t *info, void *buf, size_t size, int32_t timeout_ms)
{
    if (dmosi_semaphore_wait(q->items, 1, timeout_ms) != 0)
        return false;

    dmosi_mutex_lock(q->lock);
    slot_t *s = &q->slots[q->head];
    *info = s->info;
    if (buf != NULL)
        memcpy(buf, s->data, (s->info.len < size) ? s->info.len : size);
    q->head = (q->head + 1) % q->count;
    q->used--;
    dmosi_mutex_unlock(q->lock);

    dmosi_semaphore_post(q->room, 1);
    return true;
}

uint32_t slotq_used(slotq_t *q)
{
    dmosi_mutex_lock(q->lock);
    uint32_t used = q->used;
    dmosi_mutex_unlock(q->lock);
    return used;
}

void slotq_clear(slotq_t *q)
{
    slot_info_t info;
    while (slotq_get(q, &info, NULL, 0, 0))
        ;
}

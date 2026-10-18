#include "private.h"
#include "dmspi_types.h"
#include "dmgpio_types.h"
#include <errno.h>
#include <string.h>

/*
 * The SPI transport - its thread is the bus's only user. Every transaction
 * is FRAME_BUFFER_SIZE bytes both ways: what the host has to send (or an
 * empty frame) out, the co-processor's frame (or its empty one) in. The
 * co-processor raises Handshake when it has a transaction prepared and is
 * ready to be clocked - the host clocks only then (clocking without it
 * loses the frame) - and Data Ready when what it prepared is a real frame.
 * Both pins' interrupts only wake the thread; it reads their levels itself.
 *
 * The co-processor is restarted (its reset pin) whenever it is to be
 * reached: then its first frame is its init event, which the host answers -
 * from then on RPC and the data path go.
 */

#define RESET_PULSE_MS      5u      /* >= 2 ms: it restarts on the rising edge */
#define START_TIMEOUT_MS    3000u   /* From its restart to its init event */
#define RETRY_MS            2000u   /* Between attempts to reach it */
#define INCOMPATIBLE_MS     30000u  /* ... when it does not speak our RPC */
#define STALL_MS            5000u   /* Something to send, never ready for so long: lost */
#define IRQ_IDLE_MS         100u    /* With a pin's interrupt: its level looked at anyway */

static void close_nodes(dmdrvi_context_t context)
{
    for (int i = 0; i < FRIEND_COUNT; i++)
    {
        if (context->file[i] != NULL)
            Dmod_FileClose(context->file[i]);
        context->file[i] = NULL;
    }
}

/* SPI and Handshake are needed; without Data Ready the bus is polled, without
 * the reset pin the co-processor must start after the host */
static bool open_nodes(dmdrvi_context_t context)
{
    char *path[FRIEND_COUNT] = { NULL };

    dmosi_mutex_lock(context->lock);
    for (int i = 0; i < FRIEND_COUNT; i++)
        path[i] = (context->friend_path[i] != NULL) ? Dmod_StrDup(context->friend_path[i]) : NULL;
    dmosi_mutex_unlock(context->lock);

    bool ok = path[FRIEND_SPI] != NULL && path[FRIEND_HANDSHAKE] != NULL;
    for (int i = 0; ok && i < FRIEND_COUNT; i++)
    {
        if (path[i] == NULL || context->file[i] != NULL)
            continue;
        context->file[i] = Dmod_FileOpen(path[i], "r+");
        if (context->file[i] == NULL)
        {
            DMOD_LOG_ERROR("%s: cannot open %s\n", context->iface_name, path[i]);
            ok = (i == FRIEND_DATA_READY || i == FRIEND_RESET);
        }
    }
    for (int i = 0; i < FRIEND_COUNT; i++)
        Dmod_Free(path[i]);
    if (!ok)
        close_nodes(context);
    return ok;
}

/* The pin's level, through its node (a dmvfs call) */
static bool read_pin(dmdrvi_context_t context, friend_t pin)
{
    dmgpio_pins_mask_t high = 0;
    if (context->file[pin] == NULL || Dmod_Ioctl(context->file[pin], dmgpio_ioctl_cmd_get_high_pins_state, &high) != 0)
        return false;
    return high != 0;
}

/* The levels the interrupts keep: read from the nodes again (when they may
 * have missed an edge - at a start, after a quiet while) */
static void sync_pins(dmdrvi_context_t context)
{
    if (!context->pin_levels_by_irq)
        return;
    context->handshake_level  = read_pin(context, FRIEND_HANDSHAKE);
    context->data_ready_level = read_pin(context, FRIEND_DATA_READY);
}

static bool pin_high(dmdrvi_context_t context, friend_t pin)
{
    if (context->pin_levels_by_irq && pin == FRIEND_HANDSHAKE)
        return context->handshake_level;
    if (context->pin_levels_by_irq && pin == FRIEND_DATA_READY)
        return context->data_ready_level;
    return read_pin(context, pin);
}

/* Waits for a pin's interrupt or a frame to send; the levels are read again
 * when nothing came for so long */
static void wait_bus(dmdrvi_context_t context, uint32_t timeout_ms)
{
    if (!wait_signal(context->bus_wake, &context->bus_wake_pending, (int32_t)timeout_ms))
        sync_pins(context);
}

static void set_reset(dmdrvi_context_t context, bool high)
{
    dmgpio_pins_state_t state = high ? dmgpio_pins_state_all_high : dmgpio_pins_state_all_low;
    (void)Dmod_Ioctl(context->file[FRIEND_RESET], dmgpio_ioctl_cmd_set_pins_state, &state);
}

static void set_bus_state(dmdrvi_context_t context, bus_state_t state)
{
    dmosi_mutex_lock(context->lock);
    bus_state_t was = context->bus_state;
    context->bus_state = state;
    dmosi_mutex_unlock(context->lock);

    if (was == BUS_UP && state != BUS_UP)
        wifi_bus_changed(context, false);
}

static void restart_peer(dmdrvi_context_t context)
{
    slotq_clear(&context->tx_control);
    slotq_clear(&context->tx_data);
    context->serial_len = 0;
    context->throttled  = false;

    if (context->file[FRIEND_RESET] != NULL)
    {
        set_reset(context, false);
        dmosi_thread_sleep(RESET_PULSE_MS);
        set_reset(context, true);
    }
    else
    {
        DMOD_LOG_WARN("%s: no reset pin - the co-processor is reached only when it starts after this\n",
                      context->iface_name);
    }

    dmosi_mutex_lock(context->lock);
    context->checksum     = true;   /* Until its capabilities say */
    context->info.chip_id = DMESPHOSTED_CHIP_UNKNOWN;
    context->info.resets++;
    dmosi_mutex_unlock(context->lock);

    context->started_at = context->last_transfer_at = dmosi_get_tick_count();
    sync_pins(context);
    set_bus_state(context, BUS_STARTING);
}

/* Its init event: answered, and from then on it is up. Returns how long to
 * wait before trying again when it cannot be used (0: it can). */
static uint32_t on_init(dmdrvi_context_t context, const priv_init_t *init)
{
    uint32_t fw = init->firmware_version;
    DMOD_LOG_INFO("%s: %s (0x%02x), ESP-Hosted %u.%u.%u, capabilities 0x%02x/0x%08x\n",
                  context->iface_name, dmesphosted_chip_name(init->chip_id), init->chip_id,
                  (unsigned)((fw >> 16) & 0xFF), (unsigned)((fw >> 8) & 0xFF), (unsigned)(fw & 0xFF),
                  init->capabilities, (unsigned)init->ext_capabilities);

    if (init->has_rpc_version && init->rpc_version != PRIV_RPC_VERSION)
    {
        DMOD_LOG_ERROR("%s: the co-processor speaks RPC version %u, this driver %u - flash it with "
                       "dmesphosted's coprocessor/ firmware\n",
                       context->iface_name, init->rpc_version, PRIV_RPC_VERSION);
        return INCOMPATIBLE_MS;
    }

    uint8_t     config[PRIV_HOST_CONFIG_MAX];
    slot_info_t info = { FRAME_IF_PRIV, 0, FRAME_PRIV_EVENT, 0 };
    info.len = (uint16_t)priv_build_host_config(config, init);

    /* A start: nothing queued before it means anything to it */
    slotq_clear(&context->tx_control);
    slotq_clear(&context->tx_data);
    context->serial_len = 0;
    (void)slotq_put(&context->tx_control, &info, config, 0);

    dmosi_mutex_lock(context->lock);
    context->checksum              = (init->capabilities & PRIV_CAP_CHECKSUM) != 0;
    context->info.chip_id          = init->chip_id;
    context->info.capabilities     = init->capabilities;
    context->info.ext_capabilities = init->ext_capabilities;
    context->info.firmware_version = init->firmware_version;
    bus_state_t was = context->bus_state;
    context->bus_state = BUS_UP;
    dmosi_mutex_unlock(context->lock);

    if (was == BUS_UP)
        DMOD_LOG_WARN("%s: the co-processor restarted by itself\n", context->iface_name);
    wifi_bus_changed(context, true);
    return 0;
}

static void count(dmdrvi_context_t context, uint32_t *counter)
{
    dmosi_mutex_lock(context->lock);
    (*counter)++;
    dmosi_mutex_unlock(context->lock);
}

static void on_serial(dmdrvi_context_t context, const frame_t *f)
{
    if (context->serial_len + f->len > RPC_MAX_MESSAGE)
    {
        DMOD_LOG_ERROR("%s: an RPC message over %u bytes - dropped\n", context->iface_name, RPC_MAX_MESSAGE);
        context->serial_len = 0;
        return;
    }
    memcpy(context->serial_buf + context->serial_len, f->payload, f->len);
    context->serial_len += f->len;
    if ((f->flags & FRAME_FLAG_MORE_FRAGMENT) == 0)
    {
        rpc_received(context, context->serial_buf, context->serial_len);
        context->serial_len = 0;
    }
}

/* What came in the last transaction; true when it was a frame. *retry_ms is
 * set when the co-processor cannot be used. */
static bool on_received(dmdrvi_context_t context, bus_state_t state, uint32_t *retry_ms)
{
    frame_t        f;
    frame_result_t r = frame_decode(context->rx_buf, FRAME_BUFFER_SIZE, context->checksum, &f);

    if (r != FRAME_OK && r != FRAME_EMPTY)
    {
        count(context, &context->info.bad_frames);
        return false;
    }
    context->throttled = (f.throttle == FRAME_THROTTLE_ON);
    if (r == FRAME_EMPTY)
        return false;

    switch (f.if_type)
    {
        case FRAME_IF_PRIV:
        {
            priv_init_t init;
            if (f.priv_pkt_type == FRAME_PRIV_EVENT && priv_parse_init(f.payload, f.len, &init))
                *retry_ms = on_init(context, &init);
            break;
        }
        case FRAME_IF_SERIAL:
            if (state == BUS_UP)
                on_serial(context, &f);
            break;
        case FRAME_IF_STA:
        {
            slot_info_t info = { FRAME_IF_STA, 0, 0, f.len };
            dmosi_mutex_lock(context->lock);
            bool running = context->running;
            dmosi_mutex_unlock(context->lock);
            if (running && slotq_put(&context->rx_data, &info, f.payload, 0))
                count(context, &context->info.rx_frames);
            else
                count(context, &context->info.rx_dropped);
            break;
        }
        default:
            break;      /* SoftAP, HCI, ...: not used */
    }
    return true;
}

/* The next frame to send into tx_buf; false: an empty one */
static bool next_frame(dmdrvi_context_t context, bus_state_t state)
{
    slot_info_t info;
    uint8_t    *payload = context->tx_buf + FRAME_HEADER_SIZE;
    bool        have = slotq_get(&context->tx_control, &info, payload, FRAME_MAX_PAYLOAD, 0);

    if (!have && state == BUS_UP && !context->throttled)
        have = slotq_get(&context->tx_data, &info, payload, FRAME_MAX_PAYLOAD, 0);

    frame_t f = { 0 };
    if (have)
    {
        f.if_type       = info.if_type;
        f.flags         = info.flags;
        f.priv_pkt_type = info.priv_pkt_type;
        f.seq_num       = context->seq++;
        f.payload       = payload;
        f.len           = info.len;
    }
    dmosi_mutex_lock(context->lock);
    bool checksum = context->checksum;
    dmosi_mutex_unlock(context->lock);
    (void)frame_encode(context->tx_buf, have ? &f : NULL, checksum);
    return have;
}

void bus_wake(dmdrvi_context_t context)
{
    signal_once(context->bus_wake, &context->bus_wake_pending);
}

int bus_send_serial(dmdrvi_context_t context, const uint8_t *data, size_t len, int32_t timeout_ms)
{
    for (size_t at = 0; at < len; )
    {
        size_t      n = len - at;
        slot_info_t info = { FRAME_IF_SERIAL, 0, 0, 0 };
        if (n > FRAME_MAX_PAYLOAD)
        {
            n = FRAME_MAX_PAYLOAD;
            info.flags = FRAME_FLAG_MORE_FRAGMENT;
        }
        info.len = (uint16_t)n;
        if (!slotq_put(&context->tx_control, &info, data + at, timeout_ms))
            return -ETIMEDOUT;
        at += n;
        bus_wake(context);
    }
    return 0;
}

void bus_thread(void *arg)
{
    dmdrvi_context_t context = (dmdrvi_context_t)arg;
    uint32_t retry_at = dmosi_get_tick_count();
    uint32_t failures = 0;

    while (!context->stopping)
    {
        dmosi_mutex_lock(context->lock);
        bool changed = context->friends_changed;
        bool reset   = context->reset_requested;
        context->friends_changed = context->reset_requested = false;
        bus_state_t state = context->bus_state;
        dmosi_mutex_unlock(context->lock);

        uint32_t now = dmosi_get_tick_count();
        if (changed)
            close_nodes(context);
        if ((changed || reset) && state != BUS_DOWN)
        {
            set_bus_state(context, BUS_DOWN);
            continue;
        }

        if (state == BUS_DOWN)
        {
            int32_t wait = (int32_t)(retry_at - now);
            if (wait > 0)
            {
                (void)wait_signal(context->bus_wake, &context->bus_wake_pending, wait);
                continue;
            }
            if (open_nodes(context))
                restart_peer(context);
            else
                retry_at = now + RETRY_MS;
            continue;
        }

        if (state == BUS_STARTING && now - context->started_at > START_TIMEOUT_MS)
        {
            /* Said once, then quietly tried again */
            if (failures++ == 0)
                DMOD_LOG_WARN("%s: no answer from the co-processor - its wires, its firmware "
                              "(dmesphosted's coprocessor/README.md)? No interface until it answers\n",
                              context->iface_name);
            set_bus_state(context, BUS_DOWN);
            retry_at = now + RETRY_MS;
            continue;
        }

        bool to_send = slotq_used(&context->tx_control) != 0 ||
                       (state == BUS_UP && !context->throttled && slotq_used(&context->tx_data) != 0);
        if (!pin_high(context, FRIEND_HANDSHAKE))
        {
            if (to_send && state == BUS_UP && now - context->last_transfer_at > STALL_MS)
            {
                DMOD_LOG_ERROR("%s: the co-processor stopped answering - restarting it\n", context->iface_name);
                set_bus_state(context, BUS_DOWN);
                continue;
            }
            wait_bus(context, context->handshake_irq != NULL ? IRQ_IDLE_MS : context->poll_ms);
            continue;
        }

        /* Ready - clocked when there is something either way. Without a Data
         * Ready pin it may always have something: polled. */
        bool has_data = (context->file[FRIEND_DATA_READY] == NULL) || pin_high(context, FRIEND_DATA_READY);
        if (!to_send && !has_data)
        {
            wait_bus(context, context->data_ready_irq != NULL ? IRQ_IDLE_MS : context->poll_ms);
            continue;
        }
        if (!to_send && context->file[FRIEND_DATA_READY] == NULL &&
            dmosi_get_tick_count() - context->last_transfer_at < context->poll_ms)
        {
            wait_bus(context, context->poll_ms);
            continue;
        }

        (void)next_frame(context, state);
        dmspi_transfer_t transfer = { context->tx_buf, context->rx_buf, FRAME_BUFFER_SIZE };
        if (Dmod_Ioctl(context->file[FRIEND_SPI], dmspi_ioctl_cmd_transfer, &transfer) != 0)
        {
            DMOD_LOG_ERROR("%s: SPI transfer failed\n", context->iface_name);
            set_bus_state(context, BUS_DOWN);
            retry_at = now + RETRY_MS;
            continue;
        }
        context->last_transfer_at = dmosi_get_tick_count();

        /* Then again: its answer, the rest of what it has */
        uint32_t retry_ms = 0;
        (void)on_received(context, state, &retry_ms);
        if (retry_ms != 0)
        {
            set_bus_state(context, BUS_DOWN);
            retry_at = context->last_transfer_at + retry_ms;
            continue;
        }
        if (state == BUS_STARTING && context->bus_state == BUS_UP)
            failures = 0;
    }

    close_nodes(context);
}

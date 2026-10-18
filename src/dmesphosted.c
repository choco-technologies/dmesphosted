#define DMOD_ENABLE_REGISTRATION    ON
#include "private.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmhaman.h"
#include "dmgpio_types.h"
#ifdef DMESPHOSTED_HEAPS
#include "dmheap.h"
#endif
#include <errno.h>
#include <string.h>

#define DEFAULT_IFACE_NAME      "wlan0"
#define DEFAULT_POLL_MS         5
#define DEFAULT_PRIORITY        1
#define POLL_MS_MAX             1000
#define TX_POWER_MAX_DBM        20
#define SPI_BUFFER_ALIGNMENT    32
#define SPI_BUFFER_HEAP         "main"      /* dmod-boot's internal RAM (SRAM1): the DMA of SPI2 (DMA1) does not reach the SDRAM */
#define THREAD_STACK_SIZE       (3072 + DMOSI_THREAD_STACK_OVERHEAD)

#define TX_CONTROL_SLOTS        4       /* RPC: at most a few fragments at a time */
#define TX_DATA_SLOTS           8
#define RX_DATA_SLOTS           16      /* networkd's pump may be late: a burst waits here */

#define READ_TIMEOUT_MS         1000    /* read() waits for a frame at most so long */
#define WRITE_TIMEOUT_MS        100     /* write() waits for room at most so long */

/* A switch, not a table of name pointers: the dmod loader does not relocate
 * pointers stored in initialized data. */
static const char *friend_role(int friend)
{
    switch (friend)
    {
        case FRIEND_SPI:        return FRIEND_ROLE_SPI;
        case FRIEND_HANDSHAKE:  return FRIEND_ROLE_HANDSHAKE;
        case FRIEND_DATA_READY: return FRIEND_ROLE_DATA_READY;
        case FRIEND_RESET:      return FRIEND_ROLE_RESET;
        default:                return "";
    }
}

/* ---- The pins' interrupts ----
 *
 * Handshake and Data Ready are dmgpio inputs configured with
 * interrupt_handler=<name>; dmgpio calls that dmhaman handler from its ISR.
 * Registering under the same name here only wakes the bus thread. */

static int interrupt_handler(void *parameters, void *user_ctx)
{
    (void)parameters;
    dmdrvi_context_t context = (dmdrvi_context_t)user_ctx;

    if (is_valid_context(context))
        bus_wake(context);
    return 0;
}

/* The pin's level from what dmgpio's ISR read (its state mask): the bus
 * thread then needs no ioctl - a dmvfs call - to know it */
static bool level_of(const void *parameters)
{
    const dmgpio_interrupt_params_t *p = (const dmgpio_interrupt_params_t *)parameters;
    return (p->pins != 0) ? (p->state & p->pins) != 0 : p->state != 0;
}

static int handshake_handler(void *parameters, void *user_ctx)
{
    dmdrvi_context_t context = (dmdrvi_context_t)user_ctx;
    if (is_valid_context(context) && parameters != NULL)
    {
        context->handshake_level = level_of(parameters);
        bus_wake(context);
    }
    return 0;
}

static int data_ready_handler(void *parameters, void *user_ctx)
{
    dmdrvi_context_t context = (dmdrvi_context_t)user_ctx;
    if (is_valid_context(context) && parameters != NULL)
    {
        context->data_ready_level = level_of(parameters);
        bus_wake(context);
    }
    return 0;
}

/* ---- Configuration ---- */

static char *dup_optional(dmini_context_t ini, const char *key)
{
    const char *value = dmini_get_string(ini, NULL, key, NULL);
    return (value != NULL && value[0] != '\0') ? Dmod_StrDup(value) : NULL;
}

static bool copy_string(char *out, size_t size, const char *value)
{
    size_t len = (value != NULL) ? strlen(value) : 0;
    if (len >= size)
        return false;
    memcpy(out, value != NULL ? value : "", len + 1);
    return true;
}

/**
 * @brief Parse this driver's .ini configuration.
 *
 * Passes NULL as the section to every dmini call: dmdevfs locks the ini
 * context to this driver's own section before calling dmdrvi_create().
 * Integers are decimal (dmini_get_int).
 */
static int read_config(dmdrvi_context_t context, dmini_context_t ini)
{
    const char *name = dmini_get_string(ini, NULL, "interface", DEFAULT_IFACE_NAME);
    int poll_ms  = dmini_get_int(ini, NULL, "poll_interval_ms", DEFAULT_POLL_MS);
    int tx_power = dmini_get_int(ini, NULL, "tx_power", 0);

    context->iface_name     = Dmod_StrDup((name != NULL && name[0] != '\0') ? name : DEFAULT_IFACE_NAME);
    context->handshake_irq  = dup_optional(ini, "handshake_interrupt");
    context->data_ready_irq = dup_optional(ini, "data_ready_interrupt");
    context->priority       = dmini_get_int(ini, NULL, "thread_priority", DEFAULT_PRIORITY);
    if (context->iface_name == NULL)
        return -ENOMEM;

    if (poll_ms < 1 || poll_ms > POLL_MS_MAX)
    {
        DMOD_LOG_ERROR("Invalid poll_interval_ms %d (expected 1..%d)\n", poll_ms, POLL_MS_MAX);
        return -EINVAL;
    }
    if (tx_power < 0 || tx_power > TX_POWER_MAX_DBM)
    {
        DMOD_LOG_ERROR("Invalid tx_power %d dBm (expected 0..%d, 0: the chip's own)\n", tx_power, TX_POWER_MAX_DBM);
        return -EINVAL;
    }
    /* Off unless asked: in power save the access point keeps the frames for
     * it until a beacon - every packet waits 100 ms and more */
    const char *ps = dmini_get_string(ini, NULL, "power_save", "off");
    if (strcmp(ps, "off") == 0)
        context->power_save = RPC_WIFI_PS_NONE;
    else if (strcmp(ps, "min") == 0)
        context->power_save = RPC_WIFI_PS_MIN_MODEM;
    else if (strcmp(ps, "max") == 0)
        context->power_save = RPC_WIFI_PS_MAX_MODEM;
    else
    {
        DMOD_LOG_ERROR("Invalid power_save '%s' (expected off, min, max)\n", ps);
        return -EINVAL;
    }

    context->poll_ms  = (uint32_t)poll_ms;
    context->tx_power = tx_power * 4;   /* esp_wifi_set_max_tx_power(): quarter dBm */

    /* A network to connect to from the start - in a configuration of one's
     * own (a card's), never in a published one */
    if (!copy_string(context->network.ssid, sizeof(context->network.ssid), dmini_get_string(ini, NULL, "ssid", NULL)) ||
        !copy_string(context->network.password, sizeof(context->network.password),
                     dmini_get_string(ini, NULL, "password", NULL)))
    {
        DMOD_LOG_ERROR("Invalid ssid/password (at most %d/%d characters)\n",
                       DMDRVI_WIFI_SSID_MAX, DMDRVI_WIFI_PASSWORD_MAX);
        return -EINVAL;
    }
    memcpy(context->status.ssid, context->network.ssid, sizeof(context->status.ssid));
    return 0;
}

/* ---- Context lifetime ---- */

static void stop_threads(dmdrvi_context_t context)
{
    context->stopping = true;
    dmosi_thread_t threads[2] = { context->bus_thread, context->control_thread };
    for (int i = 0; i < 2; i++)
    {
        if (threads[i] == NULL)
            continue;
        signal_once(context->bus_wake, &context->bus_wake_pending);
        signal_once(context->control_wake, &context->control_wake_pending);
        dmosi_thread_join(threads[i]);
        dmosi_thread_destroy(threads[i]);
    }
    context->bus_thread = context->control_thread = NULL;
    if (context->process != NULL)
        dmosi_process_destroy(context->process);
    context->process = NULL;
}

/* A bus buffer: from the internal RAM's heap when there is one (it is a heap
 * of its own on dmod-boot), else the regular one */
static void *spi_buffer_alloc(dmdrvi_context_t context)
{
    (void)context;
    void *buffer = NULL;
#ifdef DMESPHOSTED_HEAPS
    dmheap_context_t *heap = dmheap_get_context_by_name(SPI_BUFFER_HEAP);
    if (heap != NULL)
        buffer = dmheap_aligned_alloc(heap, SPI_BUFFER_ALIGNMENT, FRAME_BUFFER_SIZE, DMOD_MODULE_NAME);
#endif
    return (buffer != NULL) ? buffer : Dmod_AlignedMalloc(FRAME_BUFFER_SIZE, SPI_BUFFER_ALIGNMENT);
}

static void spi_buffer_free(void *buffer)
{
    if (buffer != NULL)
        Dmod_Free(buffer);      /* (finds the heap that owns it) */
}

static void destroy_context(dmdrvi_context_t context)
{
    stop_threads(context);
    if (context->iface != NULL)
        dmnetif_unregister(context->iface);
    if (context->pin_levels_by_irq)
    {
        dmhaman_unregister_handler(context->handshake_irq, handshake_handler);
        dmhaman_unregister_handler(context->data_ready_irq, data_ready_handler);
    }
    else if (context->handshake_irq != NULL || context->data_ready_irq != NULL)
    {
        dmhaman_unregister_handler(context->handshake_irq != NULL ? context->handshake_irq : context->data_ready_irq,
                                   interrupt_handler);
        if (context->handshake_irq != NULL && context->data_ready_irq != NULL)
            dmhaman_unregister_handler(context->data_ready_irq, interrupt_handler);
    }

    slotq_deinit(&context->tx_control);
    slotq_deinit(&context->tx_data);
    slotq_deinit(&context->rx_data);

    dmosi_semaphore_t sems[] = { context->bus_wake, context->control_wake, context->rpc_done, context->scan_done };
    for (size_t i = 0; i < sizeof(sems) / sizeof(sems[0]); i++)
        if (sems[i] != NULL)
            dmosi_semaphore_destroy(sems[i]);
    dmosi_mutex_t locks[] = { context->lock, context->rpc_lock, context->scan_lock };
    for (size_t i = 0; i < sizeof(locks) / sizeof(locks[0]); i++)
        if (locks[i] != NULL)
            dmosi_mutex_destroy(locks[i]);

    for (int i = 0; i < FRIEND_COUNT; i++)
        Dmod_Free(context->friend_path[i]);
    spi_buffer_free(context->tx_buf);
    spi_buffer_free(context->rx_buf);
    Dmod_Free(context->serial_buf);
    Dmod_Free(context->rpc_buf);
    Dmod_Free(context->rpc_out);
    Dmod_Free(context->iface_name);
    Dmod_Free(context->handshake_irq);
    Dmod_Free(context->data_ready_irq);
    context->magic = 0;
    Dmod_Free(context);
}

static void bus_entry(void *arg)
{
    bus_thread(arg);
}

static void control_entry(void *arg)
{
    control_thread(arg);
}

static int start(dmdrvi_context_t context)
{
    context->lock         = dmosi_mutex_create(false);
    context->rpc_lock     = dmosi_mutex_create(false);
    context->scan_lock    = dmosi_mutex_create(false);
    context->bus_wake     = dmosi_semaphore_create(0, 1);
    context->control_wake = dmosi_semaphore_create(0, 1);
    context->rpc_done     = dmosi_semaphore_create(0, 1);
    context->scan_done    = dmosi_semaphore_create(0, 1);
    /* dmspi moves them by DMA: in memory its streams reach, whole D-cache
     * lines (32 bytes; 1600 is 50 of them) */
    context->tx_buf       = spi_buffer_alloc(context);
    context->rx_buf       = spi_buffer_alloc(context);
    context->serial_buf   = Dmod_Malloc(RPC_MAX_MESSAGE);
    context->rpc_buf      = Dmod_Malloc(RPC_MAX_MESSAGE);
    context->rpc_out      = Dmod_Malloc(RPC_MAX_MESSAGE);
    if (context->lock == NULL || context->rpc_lock == NULL || context->scan_lock == NULL ||
        context->bus_wake == NULL || context->control_wake == NULL || context->rpc_done == NULL ||
        context->scan_done == NULL || context->tx_buf == NULL || context->rx_buf == NULL ||
        context->serial_buf == NULL || context->rpc_buf == NULL || context->rpc_out == NULL ||
        slotq_init(&context->tx_control, TX_CONTROL_SLOTS) != 0 ||
        slotq_init(&context->tx_data, TX_DATA_SLOTS) != 0 ||
        slotq_init(&context->rx_data, RX_DATA_SLOTS) != 0)
        return -ENOMEM;

    context->info.chip_id = DMESPHOSTED_CHIP_UNKNOWN;
    context->state        = DMDRVI_WIFI_STATE_OFF;
    context->bus_state    = BUS_DOWN;

    /* Both pins with interrupts of their own (both edges, see the board's
     * wifi.ini): their handlers keep the levels. Otherwise the interrupts only
     * wake the bus thread, which reads the levels itself. */
    const char *hs = context->handshake_irq, *dr = context->data_ready_irq;
    context->pin_levels_by_irq = hs != NULL && dr != NULL && strcmp(hs, dr) != 0;
    if (context->pin_levels_by_irq)
    {
        if (dmhaman_register_handler(hs, handshake_handler, context) != 0 ||
            dmhaman_register_handler(dr, data_ready_handler, context) != 0)
        {
            DMOD_LOG_ERROR("Cannot register interrupt handlers '%s', '%s'\n", hs, dr);
            return -EINVAL;
        }
    }
    else if (hs != NULL || dr != NULL)
    {
        const char *name = (hs != NULL) ? hs : dr;
        if (dmhaman_register_handler(name, interrupt_handler, context) != 0)
        {
            DMOD_LOG_ERROR("Cannot register interrupt handler '%s'\n", name);
            return -EINVAL;
        }
    }

    /* Its own process: the threads are the driver's, not of whoever created it.
     * The entries are this file's static functions: the address of a global
     * one is taken through the GOT, which the dmod loader does not relocate. */
    context->process = dmosi_process_create(context->iface_name, DMOD_MODULE_NAME, NULL);
    if (context->process == NULL)
        return -ENOMEM;
    context->bus_thread = dmosi_thread_create(bus_entry, context, context->priority, THREAD_STACK_SIZE,
                                              "dmesphosted_bus", context->process);
    context->control_thread = dmosi_thread_create(control_entry, context, context->priority, THREAD_STACK_SIZE,
                                                  "dmesphosted_wifi", context->process);
    return (context->bus_thread != NULL && context->control_thread != NULL) ? 0 : -ENOMEM;
}

/* The section name becomes the node name (e.g. [wlan0] -> /dev/wlan0);
 * without a usable one the node is /dev/dmesphosted<n>. */
static void fill_dev_num(dmini_context_t ini, dmdrvi_dev_num_t *dev_num)
{
    const char *section = dmini_section_name(ini, 0);

    memset(dev_num, 0, sizeof(*dev_num));
    if (section != NULL && section[0] != '\0' && strlen(section) <= DMDRVI_ALT_NAME_MAX_LEN)
    {
        dev_num->flags = DMDRVI_NUM_ALT_NAME;
        memcpy(dev_num->alt_name, section, strlen(section) + 1);
        return;
    }
    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = 0;
}

/* ---- Names ---- */

dmod_dmesphosted_api_declaration(1.0, const char*, _chip_name, ( uint8_t chip_id ))
{
    switch (chip_id)
    {
        case DMESPHOSTED_CHIP_ESP32:   return "ESP32";
        case DMESPHOSTED_CHIP_ESP32S2: return "ESP32-S2";
        case DMESPHOSTED_CHIP_ESP32C3: return "ESP32-C3";
        case DMESPHOSTED_CHIP_ESP32S3: return "ESP32-S3";
        case DMESPHOSTED_CHIP_ESP32C2: return "ESP32-C2";
        case DMESPHOSTED_CHIP_ESP32C6: return "ESP32-C6";
        case DMESPHOSTED_CHIP_ESP32C5: return "ESP32-C5";
        case DMESPHOSTED_CHIP_UNKNOWN: return "not reached";
        default:                       return "an ESP";
    }
}

/* Appends text / a decimal number to out (size bytes in all), terminated */
static size_t put_text(char *out, size_t size, size_t at, const char *text)
{
    while (*text != '\0' && at + 1 < size)
        out[at++] = *text++;
    out[at] = '\0';
    return at;
}

static size_t put_number(char *out, size_t size, size_t at, unsigned value)
{
    char digits[10];
    size_t n = 0;
    do
    {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && n < sizeof(digits));
    while (n > 0 && at + 1 < size)
        out[at++] = digits[--n];
    out[at] = '\0';
    return at;
}

/* DMDRVI_IOCTL_WIFI_GET_INFO: what the context knows, in dmdrvi's generic form */
static void get_wifi_info(dmdrvi_context_t context, dmdrvi_wifi_info_t *out)
{
    memset(out, 0, sizeof(*out));
    put_text(out->driver, sizeof(out->driver), 0, "dmesphosted");

    dmosi_mutex_lock(context->lock);
    const dmesphosted_info_t *info = &context->info;
    if (info->chip_id != DMESPHOSTED_CHIP_UNKNOWN)
    {
        uint32_t fw = info->firmware_version;
        put_text(out->chip, sizeof(out->chip), 0, dmesphosted_chip_name(info->chip_id));
        size_t at = put_text(out->firmware, sizeof(out->firmware), 0, "ESP-Hosted ");
        at = put_number(out->firmware, sizeof(out->firmware), at, (fw >> 16) & 0xFF);
        at = put_text(out->firmware, sizeof(out->firmware), at, ".");
        at = put_number(out->firmware, sizeof(out->firmware), at, (fw >> 8) & 0xFF);
        at = put_text(out->firmware, sizeof(out->firmware), at, ".");
        put_number(out->firmware, sizeof(out->firmware), at, fw & 0xFF);
        memcpy(out->mac, info->mac, DMDRVI_WIFI_MAC_LEN);
    }
    out->rx_frames  = info->rx_frames;
    out->tx_frames  = info->tx_frames;
    out->rx_dropped = info->rx_dropped;
    out->tx_dropped = info->tx_dropped;
    out->errors     = info->bad_frames;
    out->resets     = info->resets;
    dmosi_mutex_unlock(context->lock);
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    DMOD_LOG_INFO("dmesphosted Wi-Fi driver module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("dmesphosted Wi-Fi driver module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters to dmesphosted_dmdrvi_create\n");
        return NULL;
    }

    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = DMESPHOSTED_CONTEXT_MAGIC;

    if (read_config(context, config) != 0 || start(context) != 0)
    {
        DMOD_LOG_ERROR("Failed to create DMDRVI context with provided configuration\n");
        destroy_context(context);
        return NULL;
    }

    /* The SPI bus and the pins are reported later as friends (see
     * _friend_changed()); the bus thread opens them and reaches the
     * co-processor - not dmdevfs' notifications, whose thread is not sized
     * for SPI transfers. */
    fill_dev_num(config, dev_num);
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, void, _free, ( dmdrvi_context_t context ))
{
    if (is_valid_context(context))
        destroy_context(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, void, _friend_changed,
    ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL || info->friend_role == NULL)
        return;

    for (int i = 0; i < FRIEND_COUNT; i++)
    {
        if (strcmp(info->friend_role, friend_role(i)) != 0)
            continue;

        dmosi_mutex_lock(context->lock);
        Dmod_Free(context->friend_path[i]);
        context->friend_path[i] = NULL;
        if (info->state == dmdrvi_dev_state_ready && info->node_path != NULL)
        {
            context->friend_path[i] = Dmod_StrDup(info->node_path);
            if (context->friend_path[i] == NULL)
                DMOD_LOG_ERROR("Failed to retain the %s node path\n", friend_role(i));
        }
        context->friends_changed = true;
        dmosi_mutex_unlock(context->lock);
        bus_wake(context);
        return;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, void, _path_ready, ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    if (!is_valid_context(context) || path == NULL)
    {
        DMOD_LOG_ERROR("Invalid parameters in dmesphosted_dmdrvi_path_ready\n");
        return;
    }

    context->iface = dmnetif_register(context->iface_name, path);
    if (context->iface == NULL)
    {
        DMOD_LOG_ERROR("Failed to register %s (%s) with dmnetif\n", context->iface_name, path);
        return;
    }
    DMOD_LOG_INFO("Registered %s -> %s with dmnetif\n", context->iface_name, path);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmesphosted_dmdrvi_open\n");
        return NULL;
    }
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    /* No per-handle state */
}

/* One received 802.3 frame; 0 when none came within READ_TIMEOUT_MS (or the
 * interface is not started) */
dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (offset < 0)
        return -EINVAL;
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;

    dmosi_mutex_lock(context->lock);
    bool running = context->running;
    dmosi_mutex_unlock(context->lock);
    if (!running)
        return 0;

    slot_info_t info;
    if (!slotq_get(&context->rx_data, &info, buffer, size, READ_TIMEOUT_MS))
        return 0;
    return (dmdrvi_ssize_t)((info.len < size) ? info.len : size);
}

/* One 802.3 frame to send; 0 when it cannot go (not connected, no room) */
dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (offset < 0)
        return -EINVAL;
    if (!is_valid_context(context) || buffer == NULL || size == 0)
        return 0;

    dmosi_mutex_lock(context->lock);
    bool can = context->running && context->state == DMDRVI_WIFI_STATE_CONNECTED && size <= FRAME_MAX_PAYLOAD;
    dmosi_mutex_unlock(context->lock);

    slot_info_t info = { FRAME_IF_STA, 0, 0, (uint16_t)size };
    bool sent = can && slotq_put(&context->tx_data, &info, buffer, WRITE_TIMEOUT_MS);

    dmosi_mutex_lock(context->lock);
    if (sent)
        context->info.tx_frames++;
    else
        context->info.tx_dropped++;
    dmosi_mutex_unlock(context->lock);
    if (!sent)
        return 0;
    bus_wake(context);
    return (dmdrvi_ssize_t)size;
}

static bool ioctl_needs_arg(int command)
{
    switch (command)
    {
        case DMDRVI_IOCTL_NET_SET_MAC_ADDR:
        case DMDRVI_IOCTL_NET_GET_MAC_ADDR:
        case DMDRVI_IOCTL_NET_GET_LINK_STATUS:
        case DMDRVI_IOCTL_WIFI_GET_INFO:
        case DMDRVI_IOCTL_WIFI_GET_STATUS:
        case DMDRVI_IOCTL_WIFI_CONNECT:
        case DMDRVI_IOCTL_WIFI_SCAN:
            return true;
        default:
            return false;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
    {
        DMOD_LOG_ERROR("Invalid DMDRVI context in dmesphosted_dmdrvi_ioctl\n");
        return -EINVAL;
    }
    if (arg == NULL && ioctl_needs_arg(command))
        return -EINVAL;

    switch (command)
    {
        case DMDRVI_IOCTL_NET_GET_MAC_ADDR:
        {
            static const uint8_t none[DMDRVI_WIFI_MAC_LEN] = { 0 };
            dmdrvi_net_mac_addr_t *mac = (dmdrvi_net_mac_addr_t *)arg;
            dmosi_mutex_lock(context->lock);
            bool known = memcmp(context->info.mac, none, sizeof(none)) != 0;
            memcpy(mac->addr, context->info.mac, DMDRVI_WIFI_MAC_LEN);
            dmosi_mutex_unlock(context->lock);
            return known ? 0 : -ENODEV;     /* Its MAC is known once it is reached */
        }

        case DMDRVI_IOCTL_NET_SET_MAC_ADDR:
            return -ENOTSUP;                /* The co-processor's own */

        case DMDRVI_IOCTL_NET_GET_LINK_STATUS:
        {
            dmosi_mutex_lock(context->lock);
            *(dmdrvi_net_link_status_t *)arg = (context->state == DMDRVI_WIFI_STATE_CONNECTED)
                                             ? DMDRVI_NET_LINK_UP : DMDRVI_NET_LINK_DOWN;
            dmosi_mutex_unlock(context->lock);
            return 0;
        }

        case DMDRVI_IOCTL_NET_START:
        case DMDRVI_IOCTL_NET_STOP:
        {
            dmosi_mutex_lock(context->lock);
            context->running = (command == DMDRVI_IOCTL_NET_START);
            dmosi_mutex_unlock(context->lock);
            if (command == DMDRVI_IOCTL_NET_STOP)
            {
                slotq_clear(&context->rx_data);
                slotq_clear(&context->tx_data);
            }
            return 0;
        }

        case DMDRVI_IOCTL_WIFI_GET_INFO:
            get_wifi_info(context, (dmdrvi_wifi_info_t *)arg);
            return 0;

        case DMDRVI_IOCTL_WIFI_GET_STATUS:
            wifi_get_status(context, (dmdrvi_wifi_status_t *)arg);
            return 0;

        case DMDRVI_IOCTL_WIFI_CONNECT:
            return wifi_connect(context, (const dmdrvi_wifi_connect_t *)arg);

        case DMDRVI_IOCTL_WIFI_DISCONNECT:
            return wifi_disconnect(context);

        case DMDRVI_IOCTL_WIFI_SCAN:
            return wifi_scan(context, (dmdrvi_wifi_scan_t *)arg);

        default:
            /* Including the standard block/monitor/input commands dmdevfs
             * probes every node with. */
            return -ENOTTY;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    return is_valid_context(context) ? 0 : -EINVAL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmesphosted, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;

    stat->size = (dmdrvi_size_t)0;  /* Frame by frame, not a byte store */
    stat->mode = 0666;
    return 0;
}

#ifndef DMESPHOSTED_PRIVATE_H
#define DMESPHOSTED_PRIVATE_H

#include "dmod.h"
#include "dmosi.h"
#include "dmnetif.h"
#include "dmesphosted.h"
#include "frame.h"
#include "priv.h"
#include "rpc_msg.h"

/* Magic set to "ESPH" */
#define DMESPHOSTED_CONTEXT_MAGIC   0x45535048

/* friend_role of the nodes the driver uses (all in its friends_group) */
#define FRIEND_ROLE_SPI             "spi_bus"       /* dmspi, the bus (its CS is dmspi's friend) */
#define FRIEND_ROLE_HANDSHAKE       "handshake"     /* dmgpio input: the co-processor is ready */
#define FRIEND_ROLE_DATA_READY      "data_ready"    /* dmgpio input: ... and has something */
#define FRIEND_ROLE_RESET           "reset"         /* dmgpio output: LOW >= 2 ms restarts it */

typedef enum
{
    FRIEND_SPI = 0,
    FRIEND_HANDSHAKE,
    FRIEND_DATA_READY,
    FRIEND_RESET,
    FRIEND_COUNT
} friend_t;

/* The longest RPC message (a scan's records: well under it) */
#define RPC_MAX_MESSAGE             4096u

/* ---- Frame queues (queue.c) ---- */

typedef struct
{
    uint8_t     if_type;
    uint8_t     flags;
    uint8_t     priv_pkt_type;
    uint16_t    len;
} slot_info_t;

typedef struct slot slot_t;

typedef struct
{
    slot_t             *slots;
    uint32_t            count;
    uint32_t            head;       /* Oldest */
    uint32_t            used;
    dmosi_mutex_t       lock;
    dmosi_semaphore_t   items;      /* One per slot in use */
    dmosi_semaphore_t   room;       /* One per free slot */
} slotq_t;

int  slotq_init(slotq_t *q, uint32_t count);
void slotq_deinit(slotq_t *q);
/* Copies a frame in; false when no slot was free within timeout_ms */
bool slotq_put(slotq_t *q, const slot_info_t *info, const void *data, int32_t timeout_ms);
/* The oldest frame, its data (at most size bytes) into buf (NULL: dropped) */
bool slotq_get(slotq_t *q, slot_info_t *info, void *buf, size_t size, int32_t timeout_ms);
uint32_t slotq_used(slotq_t *q);
void slotq_clear(slotq_t *q);

/* ---- The context ---- */

typedef enum
{
    BUS_DOWN = 0,                   /* Not reached: the nodes not there, or to be restarted */
    BUS_STARTING,                   /* Restarted, waiting for its init event */
    BUS_UP,                         /* The init event answered: RPC and frames go */
} bus_state_t;

struct dmdrvi_context
{
    uint32_t                magic;

    /* Configuration */
    char                   *iface_name;         /* dmnetif's name of the interface */
    char                   *handshake_irq;      /* dmhaman names of the pins' interrupts (NULL: polled) */
    char                   *data_ready_irq;
    uint32_t                poll_ms;            /* The bus looked at without an interrupt */
    int                     priority;           /* Of the threads */
    int32_t                 tx_power;           /* Quarter dBm (esp_wifi_set_max_tx_power), 0: its own */

    dmosi_mutex_t           lock;               /* What follows, unless said otherwise */

    char                   *friend_path[FRIEND_COUNT];
    bool                    friends_changed;
    bool                    reset_requested;    /* The control thread asks for a restart */

    bus_state_t             bus_state;
    bool                    checksum;           /* The co-processor checks / sends frame checksums */
    dmesphosted_info_t      info;

    dmesphosted_state_t     state;
    dmesphosted_status_t    status;
    dmesphosted_connect_t   network;            /* What to connect to ("" none) */
    bool                    wifi_started;
    uint32_t                connect_at;         /* Tick: (re)connect from then on */
    uint32_t                connect_backoff_ms;
    uint32_t                connecting_since;
    bool                    running;            /* DMDRVI_IOCTL_NET_START */
    dmnetif_iface_t         iface;

    /* Threads */
    dmosi_process_t         process;
    dmosi_thread_t          bus_thread;
    dmosi_thread_t          control_thread;
    volatile bool           stopping;
    dmosi_semaphore_t       bus_wake;           /* A pin's interrupt, a frame to send */
    dmosi_semaphore_t       control_wake;       /* A state to act on */
    volatile bool           bus_wake_pending;   /* Posted, not taken yet (see signal_once()) */
    volatile bool           control_wake_pending;

    /* The bus - the bus thread's alone */
    void                   *file[FRIEND_COUNT];
    uint8_t                *tx_buf;             /* FRAME_BUFFER_SIZE */
    uint8_t                *rx_buf;
    uint8_t                *serial_buf;         /* RPC message being put together (RPC_MAX_MESSAGE) */
    size_t                  serial_len;
    uint16_t                seq;
    bool                    throttled;
    uint32_t                started_at;         /* Tick of the restart */
    uint32_t                last_transfer_at;

    slotq_t                 tx_control;         /* PRIV and SERIAL frames */
    slotq_t                 tx_data;            /* 802.3 frames to send */
    slotq_t                 rx_data;            /* 802.3 frames received */

    /* RPC - any thread but the bus thread */
    dmosi_mutex_t           rpc_lock;           /* One call at a time */
    dmosi_mutex_t           scan_lock;          /* One scan at a time */
    dmosi_semaphore_t       rpc_done;
    dmosi_semaphore_t       scan_done;
    volatile bool           scan_done_pending;
    uint32_t                rpc_wait_id;        /* Response waited for (under lock), 0 none */
    uint32_t                rpc_uid;
    uint8_t                *rpc_buf;            /* The request out, then the response in */
    size_t                  rpc_len;
    uint8_t                *rpc_out;            /* The request wrapped */
};

static inline bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == DMESPHOSTED_CONTEXT_MAGIC;
}

/*
 * A wake-up: a binary semaphore posted at most once until it is taken - an
 * interrupt may come again and again before its thread runs, and a post on
 * a posted semaphore is an error dmosi logs. ISR-safe.
 */
static inline void signal_once(dmosi_semaphore_t sem, volatile bool *pending)
{
    if (!*pending)
    {
        *pending = true;
        (void)dmosi_semaphore_post(sem, 1);
    }
}

/* Waits for the wake-up (or the timeout); true when it came */
static inline bool wait_signal(dmosi_semaphore_t sem, volatile bool *pending, int32_t timeout_ms)
{
    bool got = dmosi_semaphore_wait(sem, 1, timeout_ms) == 0;
    *pending = false;
    return got;
}

/* ---- bus.c: the SPI transport, its thread ---- */

void bus_thread(void *arg);
/* Wakes the bus thread (an interrupt, a frame queued) */
void bus_wake(dmdrvi_context_t context);
/* Queues an RPC message (wrapped) on FRAME_IF_SERIAL, in fragments */
int  bus_send_serial(dmdrvi_context_t context, const uint8_t *data, size_t len, int32_t timeout_ms);

/* ---- rpc.c ---- */

typedef void (*rpc_build_t)(pb_writer_t *w, const void *arg);
typedef int  (*rpc_read_t)(const rpc_message_t *resp, void *arg);

/**
 * One RPC call: the request (msg_id, its payload by @p build), then waits
 * for its response and hands it to @p read (both may be NULL). Returns 0,
 * -ENODEV (no co-processor), -ETIMEDOUT, -EIO (it answered an error: the
 * esp_err_t is logged), or what @p read returns.
 */
int  rpc_call(dmdrvi_context_t context, uint32_t msg_id, rpc_build_t build, const void *build_arg,
              rpc_read_t read, void *read_arg, uint32_t timeout_ms);
/* The bus thread: an RPC message put together from its frames */
void rpc_received(dmdrvi_context_t context, const uint8_t *data, size_t len);

/* ---- wifi.c: the Wi-Fi on top ---- */

void control_thread(void *arg);
/* The bus thread: an RPC event */
void wifi_event(dmdrvi_context_t context, const rpc_message_t *event);
/* The bus thread: the co-processor (re)started or was lost */
void wifi_bus_changed(dmdrvi_context_t context, bool up);

int  wifi_connect(dmdrvi_context_t context, const dmesphosted_connect_t *network);
int  wifi_disconnect(dmdrvi_context_t context);
int  wifi_scan(dmdrvi_context_t context, dmesphosted_scan_t *scan);
void wifi_get_status(dmdrvi_context_t context, dmesphosted_status_t *status);

#endif /* DMESPHOSTED_PRIVATE_H */

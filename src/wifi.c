#include "private.h"
#include <errno.h>
#include <string.h>

/*
 * The Wi-Fi: the co-processor runs esp_wifi, the RPC calls are its API.
 * Once it is up the control thread starts it as a station (WifiInit,
 * SetWifiMode, WifiStart) and connects to the network set (WifiSetConfig,
 * WifiConnect); its events say when it is connected or lost - then it
 * connects again, waiting longer every time it fails.
 */

#define RPC_TIMEOUT_MS          2000u
#define WIFI_START_TIMEOUT_MS   5000u
#define SCAN_TIMEOUT_MS         10000
#define CONNECT_TIMEOUT_MS      20000u   /* No event for so long: tried again */
#define BACKOFF_MIN_MS          1000u
#define BACKOFF_MAX_MS          30000u
#define START_FAILURES_MAX      3        /* Then the co-processor is restarted */
#define SCAN_RECORDS_MAX        16u      /* One RPC response */
#define PASSWORD_MIN            8        /* WPA's shortest passphrase */

/* ---- Request payloads, response readers ---- */

static void build_field1(pb_writer_t *w, const void *arg)
{
    pb_put_uint(w, 1, *(const uint32_t *)arg);
}

/* Rpc_Req_IfaceMacAddrSetGet { set = 1; type = 2; mac = 3 }: get, the station's */
static void build_get_mac(pb_writer_t *w, const void *arg)
{
    (void)arg;
    pb_put_bool(w, 1, false);
    pb_put_uint(w, 2, RPC_WIFI_IF_STA);
}

static void build_wifi_init(pb_writer_t *w, const void *arg)
{
    (void)arg;
    rpc_put_wifi_init(w);
}

static void build_sta_config(pb_writer_t *w, const void *arg)
{
    const dmesphosted_connect_t *network = arg;
    rpc_put_sta_config(w, network->ssid, network->password);
}

static int read_mac(const rpc_message_t *resp, void *arg)
{
    return rpc_read_mac(resp, arg) ? 0 : -EIO;
}

static int read_number(const rpc_message_t *resp, void *arg)
{
    *(int32_t *)arg = rpc_payload_int(resp, 2);
    return 0;
}

static int read_records(const rpc_message_t *resp, void *arg)
{
    dmesphosted_scan_t *scan = arg;
    scan->count = rpc_read_ap_records(resp, scan->aps, scan->max);
    return 0;
}

static int read_ap_info(const rpc_message_t *resp, void *arg)
{
    return rpc_read_ap_info(resp, arg) ? 0 : -EIO;
}

static int call(dmdrvi_context_t context, uint32_t msg_id)
{
    return rpc_call(context, msg_id, NULL, NULL, NULL, NULL, RPC_TIMEOUT_MS);
}

/* ---- State ---- */

/* Under lock: the next attempt later, and the one after it later still */
static void schedule_retry(dmdrvi_context_t context)
{
    if (context->connect_backoff_ms < BACKOFF_MIN_MS)
        context->connect_backoff_ms = BACKOFF_MIN_MS;
    context->connect_at = dmosi_get_tick_count() + context->connect_backoff_ms;
    context->connect_backoff_ms *= 2;
    if (context->connect_backoff_ms > BACKOFF_MAX_MS)
        context->connect_backoff_ms = BACKOFF_MAX_MS;
}

/* Under lock: what a lost connection leaves */
static void clear_link(dmdrvi_context_t context)
{
    memset(context->status.bssid, 0, sizeof(context->status.bssid));
    context->status.channel = 0;
    context->status.rssi    = 0;
}

static void wake(dmdrvi_context_t context)
{
    signal_once(context->control_wake, &context->control_wake_pending);
}

void wifi_bus_changed(dmdrvi_context_t context, bool up)
{
    dmosi_mutex_lock(context->lock);
    context->wifi_started       = false;
    context->state              = up ? dmesphosted_state_starting : dmesphosted_state_off;
    context->connect_at         = dmosi_get_tick_count();
    context->connect_backoff_ms = BACKOFF_MIN_MS;
    clear_link(context);
    dmosi_mutex_unlock(context->lock);

    if (!up)
        DMOD_LOG_WARN("%s: the co-processor is gone - the link is down\n", context->iface_name);
    wake(context);
}

void wifi_event(dmdrvi_context_t context, const rpc_message_t *event)
{
    switch (event->msg_id)
    {
        case RPC_EVENT_STA_CONNECTED:
        {
            dmosi_mutex_lock(context->lock);
            (void)rpc_read_sta_connected(event, &context->status);
            context->status.reason      = 0;
            context->state              = dmesphosted_state_connected;
            context->connect_backoff_ms = BACKOFF_MIN_MS;
            char    ssid[DMESPHOSTED_SSID_MAX + 1];
            uint8_t channel = context->status.channel;
            memcpy(ssid, context->status.ssid, sizeof(ssid));
            dmosi_mutex_unlock(context->lock);
            DMOD_LOG_INFO("%s: connected to \"%s\" (channel %u)\n", context->iface_name, ssid, channel);
            break;
        }
        case RPC_EVENT_STA_DISCONNECTED:
        {
            uint16_t reason = rpc_read_sta_disconnected(event);
            dmosi_mutex_lock(context->lock);
            bool was_connected = context->state == dmesphosted_state_connected;
            context->status.reason = reason;
            clear_link(context);
            if (context->state == dmesphosted_state_connected || context->state == dmesphosted_state_connecting)
                context->state = dmesphosted_state_idle;
            if (context->network.ssid[0] != '\0')
                schedule_retry(context);
            dmosi_mutex_unlock(context->lock);
            DMOD_LOG_INFO("%s: %s (reason %u)\n", context->iface_name,
                          was_connected ? "disconnected" : "not connected", reason);
            wake(context);
            break;
        }
        case RPC_EVENT_STA_SCAN_DONE:
            signal_once(context->scan_done, &context->scan_done_pending);
            break;
        default:
            break;      /* ESPInit, heartbeats, the start / stop of its Wi-Fi */
    }
}

/* ---- The control thread ---- */

static int start_wifi(dmdrvi_context_t context)
{
    uint8_t mac[DMESPHOSTED_MAC_LEN] = { 0 };
    if (rpc_call(context, RPC_REQ_IFACE_MAC_ADDR_SET_GET, build_get_mac, NULL, read_mac, mac, RPC_TIMEOUT_MS) == 0)
    {
        dmosi_mutex_lock(context->lock);
        memcpy(context->info.mac, mac, sizeof(mac));
        dmosi_mutex_unlock(context->lock);
    }

    /* -EIO: its Wi-Fi was initialized already (it may start it itself) */
    int ret = rpc_call(context, RPC_REQ_WIFI_INIT, build_wifi_init, NULL, NULL, NULL, WIFI_START_TIMEOUT_MS);
    if (ret != 0 && ret != -EIO)
        return ret;

    uint32_t mode = RPC_WIFI_MODE_STA;
    ret = rpc_call(context, RPC_REQ_SET_WIFI_MODE, build_field1, &mode, NULL, NULL, RPC_TIMEOUT_MS);
    if (ret == 0)
        ret = rpc_call(context, RPC_REQ_WIFI_START, NULL, NULL, NULL, NULL, WIFI_START_TIMEOUT_MS);
    if (ret != 0)
        return ret;

    if (context->tx_power != 0)
    {
        uint32_t power = (uint32_t)context->tx_power;
        if (rpc_call(context, RPC_REQ_WIFI_SET_MAX_TX_POWER, build_field1, &power, NULL, NULL, RPC_TIMEOUT_MS) != 0)
            DMOD_LOG_WARN("%s: the transmit power not set\n", context->iface_name);
    }

    DMOD_LOG_INFO("%s: Wi-Fi started, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", context->iface_name,
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static int connect_network(dmdrvi_context_t context, const dmesphosted_connect_t *network)
{
    int ret = rpc_call(context, RPC_REQ_WIFI_SET_CONFIG, build_sta_config, network, NULL, NULL, RPC_TIMEOUT_MS);
    if (ret != 0)
        return ret;

    /* Connecting before the call: its events may come before its answer */
    dmosi_mutex_lock(context->lock);
    context->state            = dmesphosted_state_connecting;
    context->connecting_since = dmosi_get_tick_count();
    dmosi_mutex_unlock(context->lock);
    DMOD_LOG_INFO("%s: connecting to \"%s\"\n", context->iface_name, network->ssid);

    ret = call(context, RPC_REQ_WIFI_CONNECT);
    if (ret != 0)
    {
        dmosi_mutex_lock(context->lock);
        if (context->state == dmesphosted_state_connecting)
            context->state = dmesphosted_state_idle;
        dmosi_mutex_unlock(context->lock);
    }
    return ret;
}

void control_thread(void *arg)
{
    dmdrvi_context_t context  = (dmdrvi_context_t)arg;
    int              failures = 0;

    while (!context->stopping)
    {
        (void)wait_signal(context->control_wake, &context->control_wake_pending, 1000);
        if (context->stopping)
            break;

        uint32_t now = dmosi_get_tick_count();
        dmesphosted_connect_t network;
        dmosi_mutex_lock(context->lock);
        bool up      = context->bus_state == BUS_UP;
        bool started = context->wifi_started;
        bool due     = context->network.ssid[0] != '\0' && context->state == dmesphosted_state_idle &&
                       (int32_t)(now - context->connect_at) >= 0;
        bool stuck   = context->state == dmesphosted_state_connecting &&
                       now - context->connecting_since > CONNECT_TIMEOUT_MS;
        network = context->network;
        dmosi_mutex_unlock(context->lock);

        if (!up)
        {
            failures = 0;
            continue;
        }

        if (!started)
        {
            if (start_wifi(context) == 0)
            {
                failures = 0;
                dmosi_mutex_lock(context->lock);
                context->wifi_started = true;
                if (context->state == dmesphosted_state_starting)
                    context->state = dmesphosted_state_idle;
                dmosi_mutex_unlock(context->lock);
                wake(context);
            }
            else if (++failures >= START_FAILURES_MAX)
            {
                DMOD_LOG_ERROR("%s: its Wi-Fi does not start - restarting the co-processor\n", context->iface_name);
                failures = 0;
                dmosi_mutex_lock(context->lock);
                context->reset_requested = true;
                dmosi_mutex_unlock(context->lock);
                bus_wake(context);
            }
            continue;
        }

        if (stuck)
        {
            DMOD_LOG_WARN("%s: no answer to connecting - trying again\n", context->iface_name);
            (void)call(context, RPC_REQ_WIFI_DISCONNECT);
            dmosi_mutex_lock(context->lock);
            if (context->state == dmesphosted_state_connecting)
                context->state = dmesphosted_state_idle;
            schedule_retry(context);
            dmosi_mutex_unlock(context->lock);
            continue;
        }

        if (due && connect_network(context, &network) != 0)
        {
            dmosi_mutex_lock(context->lock);
            schedule_retry(context);
            dmosi_mutex_unlock(context->lock);
        }
    }
}

/* ---- What the ioctls do ---- */

static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0')
        n++;
    return n;
}

int wifi_connect(dmdrvi_context_t context, const dmesphosted_connect_t *network)
{
    size_t ssid_len = bounded_len(network->ssid, sizeof(network->ssid));
    size_t pass_len = bounded_len(network->password, sizeof(network->password));
    if (ssid_len == 0 || ssid_len > DMESPHOSTED_SSID_MAX || pass_len > DMESPHOSTED_PASSWORD_MAX ||
        (pass_len != 0 && pass_len < PASSWORD_MIN))
        return -EINVAL;

    dmosi_mutex_lock(context->lock);
    context->network = *network;
    context->network.ssid[ssid_len] = '\0';
    context->network.password[pass_len] = '\0';
    memcpy(context->status.ssid, context->network.ssid, ssid_len + 1);
    bool connected = context->state == dmesphosted_state_connected ||
                     context->state == dmesphosted_state_connecting;
    dmosi_mutex_unlock(context->lock);

    /* Another network: off the one it is on first */
    if (connected)
        (void)call(context, RPC_REQ_WIFI_DISCONNECT);

    dmosi_mutex_lock(context->lock);
    if (context->state == dmesphosted_state_connected || context->state == dmesphosted_state_connecting)
        context->state = dmesphosted_state_idle;
    context->connect_at         = dmosi_get_tick_count();
    context->connect_backoff_ms = BACKOFF_MIN_MS;
    dmosi_mutex_unlock(context->lock);
    wake(context);
    return 0;
}

int wifi_disconnect(dmdrvi_context_t context)
{
    dmosi_mutex_lock(context->lock);
    memset(&context->network, 0, sizeof(context->network));
    context->status.ssid[0] = '\0';
    bool connected = context->state == dmesphosted_state_connected ||
                     context->state == dmesphosted_state_connecting;
    dmosi_mutex_unlock(context->lock);

    int ret = connected ? call(context, RPC_REQ_WIFI_DISCONNECT) : 0;

    dmosi_mutex_lock(context->lock);
    if (context->state == dmesphosted_state_connected || context->state == dmesphosted_state_connecting)
        context->state = dmesphosted_state_idle;
    clear_link(context);
    dmosi_mutex_unlock(context->lock);
    return ret;
}

int wifi_scan(dmdrvi_context_t context, dmesphosted_scan_t *scan)
{
    if (scan->aps == NULL && scan->max != 0)
        return -EINVAL;
    scan->count = 0;

    dmosi_mutex_lock(context->lock);
    bool started = context->wifi_started;
    dmosi_mutex_unlock(context->lock);
    if (!started)
        return -ENODEV;

    dmosi_mutex_lock(context->scan_lock);
    (void)wait_signal(context->scan_done, &context->scan_done_pending, 0);  /* An earlier scan's */

    /* Rpc_Req_WifiScanStart, empty: every channel, its default times, not
     * waiting for it - its end is an event */
    int ret = call(context, RPC_REQ_WIFI_SCAN_START);
    if (ret == -EIO)
        ret = -EBUSY;   /* esp_wifi does not scan while it connects */
    if (ret == 0 && !wait_signal(context->scan_done, &context->scan_done_pending, SCAN_TIMEOUT_MS))
        ret = -ETIMEDOUT;

    int32_t found = 0;
    if (ret == 0)
        ret = rpc_call(context, RPC_REQ_WIFI_SCAN_GET_AP_NUM, NULL, NULL, read_number, &found, RPC_TIMEOUT_MS);

    uint32_t want = (found > 0) ? (uint32_t)found : 0;
    if (want > scan->max)
        want = scan->max;
    if (want > SCAN_RECORDS_MAX)
        want = SCAN_RECORDS_MAX;
    if (ret == 0 && want != 0)
    {
        dmesphosted_scan_t part = { scan->aps, (uint16_t)want, 0 };
        ret = rpc_call(context, RPC_REQ_WIFI_SCAN_GET_AP_RECORDS, build_field1, &want, read_records, &part,
                       RPC_TIMEOUT_MS);
        scan->count = part.count;
    }
    dmosi_mutex_unlock(context->scan_lock);
    return ret;
}

void wifi_get_status(dmdrvi_context_t context, dmesphosted_status_t *status)
{
    dmosi_mutex_lock(context->lock);
    *status       = context->status;
    status->state = context->state;
    dmosi_mutex_unlock(context->lock);

    dmesphosted_ap_t ap;
    if (status->state == dmesphosted_state_connected &&
        rpc_call(context, RPC_REQ_WIFI_STA_GET_AP_INFO, NULL, NULL, read_ap_info, &ap, RPC_TIMEOUT_MS) == 0)
        status->rssi = ap.rssi;
}

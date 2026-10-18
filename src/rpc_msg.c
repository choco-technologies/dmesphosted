#include "rpc_msg.h"
#include <string.h>

/* Rpc's fields */
#define RPC_FIELD_MSG_TYPE      1u
#define RPC_FIELD_MSG_ID        2u
#define RPC_FIELD_UID           3u

/* esp_wifi_init()'s check of the config (WIFI_INIT_CONFIG_MAGIC) */
#define WIFI_INIT_CONFIG_MAGIC  0x1F2F3F4F

void rpc_begin_request(pb_writer_t *w, uint32_t msg_id, uint32_t uid)
{
    pb_put_uint(w, RPC_FIELD_MSG_TYPE, RPC_TYPE_REQ);
    pb_put_uint(w, RPC_FIELD_MSG_ID, msg_id);
    pb_put_uint(w, RPC_FIELD_UID, uid);
    pb_begin(w, msg_id);
}

size_t rpc_end_request(pb_writer_t *w)
{
    pb_end(w);
    return pb_writer_done(w);
}

/*
 * Rpc_Req_WifiInit { wifi_init_config cfg = 1 }: the co-processor takes it as
 * is, so it is WIFI_INIT_CONFIG_DEFAULT() of a co-processor's firmware (the
 * values Espressif's own hosts send). Not configuration: esp_wifi_init()
 * checks the magic and the buffer counts.
 */
void rpc_put_wifi_init(pb_writer_t *w)
{
    pb_begin(w, 1);
    pb_put_int(w, 1, 10);           /* static_rx_buf_num */
    pb_put_int(w, 2, 32);           /* dynamic_rx_buf_num */
    pb_put_int(w, 3, 1);            /* tx_buf_type: dynamic */
    pb_put_int(w, 5, 32);           /* dynamic_tx_buf_num */
    pb_put_int(w, 8, 1);            /* ampdu_rx_enable */
    pb_put_int(w, 9, 1);            /* ampdu_tx_enable */
    pb_put_int(w, 11, 1);           /* nvs_enable */
    pb_put_int(w, 13, 6);           /* rx_ba_win */
    pb_put_int(w, 15, 752);         /* beacon_max_len */
    pb_put_int(w, 16, 32);          /* mgmt_sbuf_num */
    pb_put_uint(w, 17, 1);          /* feature_caps */
    pb_put_bool(w, 18, true);       /* sta_disconnected_pm */
    pb_put_int(w, 19, 7);           /* espnow_max_encrypt_num */
    pb_put_int(w, 20, WIFI_INIT_CONFIG_MAGIC);
    pb_put_int(w, 22, 5);           /* rx_mgmt_buf_num */
    pb_put_int(w, 23, 3);           /* tx_hetb_queue_num */
    pb_end(w);
}

/*
 * Rpc_Req_WifiSetConfig { iface = 1; wifi_config cfg = 2 { oneof: sta = 2 {
 * ssid = 1; password = 2; pmf_cfg = 10 { capable = 1 } } } }. PMF capable
 * (not required): a WPA2/WPA3 access point takes it either way.
 */
void rpc_put_sta_config(pb_writer_t *w, const char *ssid, const char *password)
{
    pb_put_uint(w, 1, RPC_WIFI_IF_STA);
    pb_begin(w, 2);
    pb_begin(w, 2);
    pb_put_bytes(w, 1, ssid, strlen(ssid));
    pb_put_bytes(w, 2, password, strlen(password));
    pb_begin(w, 10);
    pb_put_bool(w, 1, true);
    pb_end(w);
    pb_end(w);
    pb_end(w);
}

static void put_le16(uint8_t *p, size_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

size_t rpc_wrap(uint8_t *out, size_t size, const char *endpoint, const uint8_t *pb, size_t pb_len)
{
    size_t ep_len = strlen(endpoint);
    size_t total  = 3u + ep_len + 3u + pb_len;
    if (total > size || pb_len > UINT16_MAX)
        return 0;

    size_t p = 0;
    out[p++] = RPC_TLV_EP_NAME;
    put_le16(out + p, ep_len);
    p += 2;
    memcpy(out + p, endpoint, ep_len);
    p += ep_len;
    out[p++] = RPC_TLV_DATA;
    put_le16(out + p, pb_len);
    p += 2;
    memcpy(out + p, pb, pb_len);
    return total;
}

bool rpc_unwrap(const uint8_t *in, size_t len, char endpoint[RPC_EP_NAME_MAX],
                const uint8_t **pb, size_t *pb_len)
{
    bool have_ep = false, have_data = false;
    size_t p = 0;

    endpoint[0] = '\0';
    while (p + 3u <= len)
    {
        uint8_t type = in[p];
        size_t  tlen = (size_t)in[p + 1] | ((size_t)in[p + 2] << 8);
        p += 3;
        if (tlen > len - p)
            return false;
        if (type == RPC_TLV_EP_NAME)
        {
            size_t n = (tlen < RPC_EP_NAME_MAX - 1u) ? tlen : RPC_EP_NAME_MAX - 1u;
            memcpy(endpoint, in + p, n);
            endpoint[n] = '\0';
            have_ep = true;
        }
        else if (type == RPC_TLV_DATA)
        {
            *pb     = in + p;
            *pb_len = tlen;
            have_data = true;
        }
        p += tlen;
    }
    return have_ep && have_data;
}

bool rpc_decode(const uint8_t *pb, size_t len, rpc_message_t *msg)
{
    pb_reader_t r;
    pb_field_t  f;
    bool        have_id = false;

    memset(msg, 0, sizeof(*msg));
    pb_reader_init(&r, pb, len);
    while (pb_next(&r, &f))
    {
        if (f.wire == PB_WIRE_VARINT && f.number == RPC_FIELD_MSG_TYPE)
            msg->type = (uint32_t)f.value;
        else if (f.wire == PB_WIRE_VARINT && f.number == RPC_FIELD_MSG_ID)
        {
            msg->msg_id = (uint32_t)f.value;
            have_id = true;
        }
        else if (f.wire == PB_WIRE_VARINT && f.number == RPC_FIELD_UID)
            msg->uid = (uint32_t)f.value;
        else if (f.wire == PB_WIRE_LEN && f.number > RPC_FIELD_UID)
        {
            msg->payload     = f.data;
            msg->payload_len = f.len;
        }
    }
    /* The payload is the one numbered as msg_id (the oneof has just one) */
    return have_id;
}

/* The first field numbered @p number of a message (data, len) */
static bool find_field(const uint8_t *data, size_t len, uint32_t number, pb_field_t *out)
{
    pb_reader_t r;
    pb_field_t  f;
    bool        found = false;

    pb_reader_init(&r, data, len);
    while (pb_next(&r, &f))
    {
        if (f.number == number && !found)
        {
            *out  = f;
            found = true;
        }
    }
    return found;
}

int32_t rpc_payload_int(const rpc_message_t *msg, uint32_t field)
{
    pb_field_t f;
    if (msg->payload == NULL || !find_field(msg->payload, msg->payload_len, field, &f) ||
        f.wire != PB_WIRE_VARINT)
        return 0;
    return pb_int32(&f);
}

int32_t rpc_resp_code(const rpc_message_t *msg)
{
    return rpc_payload_int(msg, 1);
}

bool rpc_read_mac(const rpc_message_t *msg, uint8_t mac[DMESPHOSTED_MAC_LEN])
{
    pb_field_t f;
    if (msg->payload == NULL || !find_field(msg->payload, msg->payload_len, 4, &f) ||
        f.wire != PB_WIRE_LEN || f.len != DMESPHOSTED_MAC_LEN)
        return false;
    memcpy(mac, f.data, DMESPHOSTED_MAC_LEN);
    return true;
}

static void copy_ssid(char out[DMESPHOSTED_SSID_MAX + 1], const pb_field_t *f)
{
    size_t n = (f->len < DMESPHOSTED_SSID_MAX) ? f->len : DMESPHOSTED_SSID_MAX;
    memcpy(out, f->data, n);
    out[n] = '\0';
}

/* wifi_event_sta_connected { ssid = 1; ssid_len = 2; bssid = 3; channel = 4; ... } */
bool rpc_read_sta_connected(const rpc_message_t *msg, dmesphosted_status_t *status)
{
    pb_field_t  ev, f;
    pb_reader_t r;

    if (msg->payload == NULL || !find_field(msg->payload, msg->payload_len, 2, &ev) ||
        ev.wire != PB_WIRE_LEN)
        return false;

    pb_reader_sub(&r, &ev);
    while (pb_next(&r, &f))
    {
        if (f.number == 1 && f.wire == PB_WIRE_LEN)
            copy_ssid(status->ssid, &f);
        else if (f.number == 3 && f.wire == PB_WIRE_LEN && f.len == DMESPHOSTED_MAC_LEN)
            memcpy(status->bssid, f.data, DMESPHOSTED_MAC_LEN);
        else if (f.number == 4 && f.wire == PB_WIRE_VARINT)
            status->channel = (uint8_t)f.value;
    }
    return true;
}

/* wifi_event_sta_disconnected { ssid = 1; ssid_len = 2; bssid = 3; reason = 4; rssi = 5 } */
uint16_t rpc_read_sta_disconnected(const rpc_message_t *msg)
{
    pb_field_t ev, f;
    if (msg->payload == NULL || !find_field(msg->payload, msg->payload_len, 2, &ev) ||
        ev.wire != PB_WIRE_LEN || !find_field(ev.data, ev.len, 4, &f) || f.wire != PB_WIRE_VARINT)
        return 0;
    return (uint16_t)f.value;
}

/* wifi_ap_record { bssid = 1; ssid = 2; primary = 3; second = 4; rssi = 5; authmode = 6; ... } */
static void read_ap_record(const pb_field_t *record, dmesphosted_ap_t *ap)
{
    pb_reader_t r;
    pb_field_t  f;

    memset(ap, 0, sizeof(*ap));
    pb_reader_sub(&r, record);
    while (pb_next(&r, &f))
    {
        if (f.number == 1 && f.wire == PB_WIRE_LEN && f.len == DMESPHOSTED_MAC_LEN)
            memcpy(ap->bssid, f.data, DMESPHOSTED_MAC_LEN);
        else if (f.number == 2 && f.wire == PB_WIRE_LEN)
            copy_ssid(ap->ssid, &f);
        else if (f.number == 3 && f.wire == PB_WIRE_VARINT)
            ap->channel = (uint8_t)f.value;
        else if (f.number == 5 && f.wire == PB_WIRE_VARINT)
            ap->rssi = (int8_t)pb_int32(&f);
        else if (f.number == 6 && f.wire == PB_WIRE_VARINT)
            ap->authmode = (uint8_t)f.value;
    }
}

/* Rpc_Resp_WifiScanGetApRecords { resp = 1; number = 2; repeated ap_records = 3 } */
uint16_t rpc_read_ap_records(const rpc_message_t *msg, dmesphosted_ap_t *aps, uint16_t max)
{
    pb_reader_t r;
    pb_field_t  f;
    uint16_t    n = 0;

    if (msg->payload == NULL)
        return 0;
    pb_reader_init(&r, msg->payload, msg->payload_len);
    while (n < max && pb_next(&r, &f))
    {
        if (f.number == 3 && f.wire == PB_WIRE_LEN)
            read_ap_record(&f, &aps[n++]);
    }
    return n;
}

/* Rpc_Resp_WifiStaGetApInfo { resp = 1; ap_record = 2 } */
bool rpc_read_ap_info(const rpc_message_t *msg, dmesphosted_ap_t *ap)
{
    pb_field_t f;
    if (msg->payload == NULL || !find_field(msg->payload, msg->payload_len, 2, &f) ||
        f.wire != PB_WIRE_LEN)
        return false;
    read_ap_record(&f, ap);
    return true;
}

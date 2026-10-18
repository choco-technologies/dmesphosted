#define DMOD_ENABLE_REGISTRATION ON
#define ENABLE_DIF_REGISTRATIONS ON
#include "dmod_test.h"
#include "dmesphosted.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "frame.h"
#include "pb.h"
#include "priv.h"
#include "rpc_msg.h"
#include "rpc_fixtures.h"
#include <errno.h>
#include <string.h>

static uint8_t g_buf[FRAME_BUFFER_SIZE];
static uint8_t g_pb[512];

void dmod_test_setup(void)
{
    memset(g_buf, 0, sizeof(g_buf));
    memset(g_pb, 0, sizeof(g_pb));
}

void dmod_test_teardown(void)
{
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != 0)
            return false;
    return true;
}

/* ---- Frames ---- */

DMOD_TEST_STEP(frame_encodes_header_and_checksum)
{
    static const uint8_t payload[3] = { 0xAA, 0xBB, 0xCC };
    frame_t f = { 0 };
    f.if_type = FRAME_IF_STA;
    f.seq_num = 0x0102;
    f.payload = payload;
    f.len     = sizeof(payload);

    DMOD_TEST_EXPECT_EQ(frame_encode(g_buf, &f, true), 15);
    /* if_type 1 | flags | len 3 | offset 12 | checksum | seq 0x0102 | throttle | priv type */
    DMOD_TEST_EXPECT_EQ(g_buf[0], 0x01);
    DMOD_TEST_EXPECT_EQ(g_buf[2], 0x03);
    DMOD_TEST_EXPECT_EQ(g_buf[4], 0x0C);
    DMOD_TEST_EXPECT_EQ(g_buf[8], 0x02);
    DMOD_TEST_EXPECT_EQ(g_buf[9], 0x01);
    /* 0x01 + 0x03 + 0x0C + 0x02 + 0x01 + 0xAA + 0xBB + 0xCC = 0x0244, LE */
    DMOD_TEST_EXPECT_EQ(g_buf[6], 0x44);
    DMOD_TEST_EXPECT_EQ(g_buf[7], 0x02);
    DMOD_TEST_EXPECT_EQ(g_buf[12], 0xAA);
    DMOD_TEST_EXPECT_TRUE(all_zero(g_buf + 15, FRAME_BUFFER_SIZE - 15));
}

DMOD_TEST_STEP(frame_round_trip_and_checksum)
{
    uint8_t payload[100];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 7);
    frame_t f = { 0 }, d;
    f.if_type = FRAME_IF_SERIAL;
    f.flags   = FRAME_FLAG_MORE_FRAGMENT;
    f.seq_num = 7;
    f.payload = payload;
    f.len     = sizeof(payload);
    DMOD_TEST_EXPECT_EQ(frame_encode(g_buf, &f, true), FRAME_HEADER_SIZE + sizeof(payload));

    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, true, &d), FRAME_OK);
    DMOD_TEST_EXPECT_EQ(d.if_type, FRAME_IF_SERIAL);
    DMOD_TEST_EXPECT_EQ(d.flags, FRAME_FLAG_MORE_FRAGMENT);
    DMOD_TEST_EXPECT_EQ(d.seq_num, 7);
    DMOD_TEST_EXPECT_EQ(d.len, sizeof(payload));
    DMOD_TEST_EXPECT_TRUE(d.payload == g_buf + FRAME_HEADER_SIZE);
    DMOD_TEST_EXPECT_EQ(memcmp(d.payload, payload, sizeof(payload)), 0);

    g_buf[FRAME_HEADER_SIZE + 50] ^= 0x10;
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, true, &d), FRAME_BAD_CHECKSUM);
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, false, &d), FRAME_OK);
}

DMOD_TEST_STEP(frame_empty_and_bad_headers)
{
    frame_t d;
    DMOD_TEST_EXPECT_EQ(frame_encode(g_buf, NULL, true), FRAME_HEADER_SIZE);
    DMOD_TEST_EXPECT_EQ(g_buf[0], FRAME_IF_DUMMY);
    DMOD_TEST_EXPECT_EQ(g_buf[4], FRAME_HEADER_SIZE);
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, true, &d), FRAME_EMPTY);

    /* What a co-processor not running (MISO floating high) gives */
    memset(g_buf, 0xFF, sizeof(g_buf));
    DMOD_TEST_EXPECT_NE(frame_decode(g_buf, FRAME_BUFFER_SIZE, true, &d), FRAME_OK);

    memset(g_buf, 0, sizeof(g_buf));
    g_buf[0] = FRAME_IF_STA;
    g_buf[2] = 0xD0;        /* len 2000 */
    g_buf[3] = 0x07;
    g_buf[4] = FRAME_HEADER_SIZE;
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, false, &d), FRAME_BAD_HEADER);
    g_buf[2] = 10;
    g_buf[3] = 0;
    g_buf[4] = 4;           /* offset inside the header */
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, false, &d), FRAME_BAD_HEADER);

    frame_t big = { 0 };
    big.if_type = FRAME_IF_STA;
    big.payload = g_pb;
    big.len     = FRAME_MAX_PAYLOAD + 1;
    DMOD_TEST_EXPECT_EQ(frame_encode(g_buf, &big, true), 0);
}

DMOD_TEST_STEP(frame_payload_in_place)
{
    for (size_t i = 0; i < 64; i++)
        g_buf[FRAME_HEADER_SIZE + i] = (uint8_t)(0x40 + i);
    frame_t f = { 0 }, d;
    f.if_type = FRAME_IF_STA;
    f.payload = g_buf + FRAME_HEADER_SIZE;
    f.len     = 64;
    DMOD_TEST_EXPECT_EQ(frame_encode(g_buf, &f, true), FRAME_HEADER_SIZE + 64);
    DMOD_TEST_EXPECT_EQ(frame_decode(g_buf, FRAME_BUFFER_SIZE, true, &d), FRAME_OK);
    DMOD_TEST_EXPECT_EQ(d.payload[0], 0x40);
    DMOD_TEST_EXPECT_EQ(d.payload[63], 0x40 + 63);
}

/* ---- protobuf ---- */

DMOD_TEST_STEP(pb_writes_varints)
{
    pb_writer_t w;
    pb_writer_init(&w, g_pb, sizeof(g_pb));
    pb_put_uint(&w, 1, 300);        /* 08 ac 02 */
    pb_put_int(&w, 2, -1);          /* 10 ff x9 01: int32 sign-extended */
    pb_put_bool(&w, 16, true);      /* 80 01 01 */
    pb_put_uint(&w, 3, 0);          /* default: not written */
    pb_put_bytes(&w, 4, "", 0);

    static const uint8_t expected[] = {
        0x08, 0xAC, 0x02,
        0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
        0x80, 0x01, 0x01,
    };
    DMOD_TEST_EXPECT_EQ(pb_writer_done(&w), sizeof(expected));
    DMOD_TEST_EXPECT_EQ(memcmp(g_pb, expected, sizeof(expected)), 0);

    pb_reader_t r;
    pb_field_t  f;
    pb_reader_init(&r, g_pb, pb_writer_done(&w));
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(f.number, 1);
    DMOD_TEST_EXPECT_EQ(f.value, 300);
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(pb_int32(&f), -1);
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(f.number, 16);
    DMOD_TEST_EXPECT_FALSE(pb_next(&r, &f));
}

DMOD_TEST_STEP(pb_nested_messages_longer_than_127)
{
    uint8_t blob[200];
    memset(blob, 0x5A, sizeof(blob));
    pb_writer_t w;
    pb_writer_init(&w, g_pb, sizeof(g_pb));
    pb_begin(&w, 5);
    pb_begin(&w, 2);
    pb_put_bytes(&w, 1, blob, sizeof(blob));    /* 1 + 2 + 200 = 203: a 2-byte length */
    pb_end(&w);
    pb_put_uint(&w, 3, 9);
    pb_end(&w);
    size_t n = pb_writer_done(&w);
    /* 2a <len 2> [ 12 <cb 01> [ 0a <c8 01> blob ] 18 09 ] */
    DMOD_TEST_EXPECT_EQ(n, 1 + 2 + (1 + 2 + 203) + 2);
    DMOD_TEST_EXPECT_EQ(g_pb[0], 0x2A);
    DMOD_TEST_EXPECT_EQ(g_pb[3], 0x12);
    DMOD_TEST_EXPECT_EQ(g_pb[4], 0xCB);
    DMOD_TEST_EXPECT_EQ(g_pb[5], 0x01);

    pb_reader_t r, sub, subsub;
    pb_field_t  f, g, h;
    pb_reader_init(&r, g_pb, n);
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(f.wire, PB_WIRE_LEN);
    pb_reader_sub(&sub, &f);
    DMOD_TEST_EXPECT_TRUE(pb_next(&sub, &g));
    pb_reader_sub(&subsub, &g);
    DMOD_TEST_EXPECT_TRUE(pb_next(&subsub, &h));
    DMOD_TEST_EXPECT_EQ(h.len, sizeof(blob));
    DMOD_TEST_EXPECT_EQ(memcmp(h.data, blob, sizeof(blob)), 0);
    DMOD_TEST_EXPECT_TRUE(pb_next(&sub, &g));
    DMOD_TEST_EXPECT_EQ(g.number, 3);
    DMOD_TEST_EXPECT_EQ(g.value, 9);
}

DMOD_TEST_STEP(pb_overflow_and_broken_data)
{
    uint8_t     small[8];
    pb_writer_t w;
    pb_writer_init(&w, small, sizeof(small));
    pb_put_bytes(&w, 1, "0123456789", 10);
    DMOD_TEST_EXPECT_EQ(pb_writer_done(&w), 0);

    pb_writer_init(&w, small, sizeof(small));
    pb_begin(&w, 1);
    DMOD_TEST_EXPECT_EQ(pb_writer_done(&w), 0);   /* Left open */

    /* fixed32, fixed64 skipped; a length past the end is broken */
    static const uint8_t data[] = {
        0x0D, 1, 2, 3, 4,                   /* 1: fixed32 */
        0x11, 1, 2, 3, 4, 5, 6, 7, 8,       /* 2: fixed64 */
        0x18, 0x05,                         /* 3: 5 */
        0x22, 0x10, 0x01,                   /* 4: 16 bytes - only 1 there */
    };
    pb_reader_t r;
    pb_field_t  f;
    pb_reader_init(&r, data, sizeof(data));
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(f.value, 0x04030201);
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_TRUE(pb_next(&r, &f));
    DMOD_TEST_EXPECT_EQ(f.value, 5);
    DMOD_TEST_EXPECT_FALSE(pb_next(&r, &f));
}

/* ---- RPC messages, against Google's protobuf (rpc_fixtures.h) ---- */

static size_t request(uint32_t msg_id, uint32_t uid, void (*fill)(pb_writer_t *))
{
    pb_writer_t w;
    pb_writer_init(&w, g_pb, sizeof(g_pb));
    rpc_begin_request(&w, msg_id, uid);
    if (fill != NULL)
        fill(&w);
    return rpc_end_request(&w);
}

static void fill_mode(pb_writer_t *w)      { pb_put_uint(w, 1, RPC_WIFI_MODE_STA); }
static void fill_records(pb_writer_t *w)   { pb_put_uint(w, 1, 16); }
static void fill_tx_power(pb_writer_t *w)  { pb_put_uint(w, 1, 80); }
static void fill_ps_none(pb_writer_t *w)   { pb_put_uint(w, 1, RPC_WIFI_PS_NONE); }
static void fill_ps_min(pb_writer_t *w)    { pb_put_uint(w, 1, RPC_WIFI_PS_MIN_MODEM); }
static void fill_init(pb_writer_t *w)      { rpc_put_wifi_init(w); }
static void fill_sta(pb_writer_t *w)       { rpc_put_sta_config(w, "Home Network", "secret123"); }
static void fill_sta_open(pb_writer_t *w)  { rpc_put_sta_config(w, "Cafe", ""); }
static void fill_get_mac(pb_writer_t *w)
{
    pb_put_bool(w, 1, false);
    pb_put_uint(w, 2, RPC_WIFI_IF_STA);
}

#define EXPECT_REQUEST(fixture, msg_id, uid, fill) \
    do { \
        size_t n_ = request((msg_id), (uid), (fill)); \
        DMOD_TEST_EXPECT_EQ(n_, sizeof(fixture)); \
        DMOD_TEST_EXPECT_EQ(memcmp(g_pb, (fixture), sizeof(fixture)), 0); \
    } while (0)

DMOD_TEST_STEP(rpc_requests_match_protobuf)
{
    EXPECT_REQUEST(fx_req_get_mac,         RPC_REQ_IFACE_MAC_ADDR_SET_GET,   1, fill_get_mac);
    EXPECT_REQUEST(fx_req_wifi_init,       RPC_REQ_WIFI_INIT,                2, fill_init);
    EXPECT_REQUEST(fx_req_set_mode,        RPC_REQ_SET_WIFI_MODE,            3, fill_mode);
    EXPECT_REQUEST(fx_req_wifi_start,      RPC_REQ_WIFI_START,               4, NULL);
    EXPECT_REQUEST(fx_req_set_config,      RPC_REQ_WIFI_SET_CONFIG,          5, fill_sta);
    EXPECT_REQUEST(fx_req_set_config_open, RPC_REQ_WIFI_SET_CONFIG,          6, fill_sta_open);
    EXPECT_REQUEST(fx_req_scan_start,      RPC_REQ_WIFI_SCAN_START,          7, NULL);
    EXPECT_REQUEST(fx_req_get_ap_records,  RPC_REQ_WIFI_SCAN_GET_AP_RECORDS, 8, fill_records);
    EXPECT_REQUEST(fx_req_set_tx_power,    RPC_REQ_WIFI_SET_MAX_TX_POWER,    9, fill_tx_power);
    EXPECT_REQUEST(fx_req_set_ps_none,     RPC_REQ_WIFI_SET_PS,             10, fill_ps_none);
    EXPECT_REQUEST(fx_req_set_ps_min,      RPC_REQ_WIFI_SET_PS,             11, fill_ps_min);
}

DMOD_TEST_STEP(rpc_reads_responses)
{
    rpc_message_t m;
    uint8_t       mac[DMESPHOSTED_MAC_LEN];
    static const uint8_t expected_mac[] = { 0x9C, 0xCC, 0x01, 0xA8, 0x1E, 0x68 };

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_resp_get_mac, sizeof(fx_resp_get_mac), &m));
    DMOD_TEST_EXPECT_EQ(m.type, RPC_TYPE_RESP);
    DMOD_TEST_EXPECT_EQ(m.msg_id, RPC_REQ_IFACE_MAC_ADDR_SET_GET + RPC_RESP_OFFSET);
    DMOD_TEST_EXPECT_EQ(m.uid, 1);
    DMOD_TEST_EXPECT_EQ(rpc_resp_code(&m), 0);
    DMOD_TEST_EXPECT_TRUE(rpc_read_mac(&m, mac));
    DMOD_TEST_EXPECT_EQ(memcmp(mac, expected_mac, sizeof(mac)), 0);

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_resp_wifi_init_error, sizeof(fx_resp_wifi_init_error), &m));
    DMOD_TEST_EXPECT_EQ(m.msg_id, RPC_REQ_WIFI_INIT + RPC_RESP_OFFSET);
    DMOD_TEST_EXPECT_EQ(rpc_resp_code(&m), 0x3001);

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_resp_ap_num, sizeof(fx_resp_ap_num), &m));
    DMOD_TEST_EXPECT_EQ(rpc_payload_int(&m, 2), 3);

    dmesphosted_ap_t aps[3];
    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_resp_ap_records, sizeof(fx_resp_ap_records), &m));
    DMOD_TEST_EXPECT_EQ(rpc_read_ap_records(&m, aps, 3), 2);
    DMOD_TEST_EXPECT_EQ(strcmp(aps[0].ssid, "Home Network"), 0);
    DMOD_TEST_EXPECT_EQ(aps[0].bssid[0], 0xF4);
    DMOD_TEST_EXPECT_EQ(aps[0].bssid[5], 0x33);
    DMOD_TEST_EXPECT_EQ(aps[0].channel, 6);
    DMOD_TEST_EXPECT_EQ(aps[0].rssi, -47);
    DMOD_TEST_EXPECT_EQ(aps[0].authmode, 3);
    DMOD_TEST_EXPECT_EQ(strcmp(aps[1].ssid, "Cafe"), 0);
    DMOD_TEST_EXPECT_EQ(aps[1].channel, 11);
    DMOD_TEST_EXPECT_EQ(aps[1].rssi, -80);
    DMOD_TEST_EXPECT_EQ(aps[1].authmode, 0);
    DMOD_TEST_EXPECT_EQ(rpc_read_ap_records(&m, aps, 1), 1);

    dmesphosted_ap_t ap;
    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_resp_ap_info, sizeof(fx_resp_ap_info), &m));
    DMOD_TEST_EXPECT_TRUE(rpc_read_ap_info(&m, &ap));
    DMOD_TEST_EXPECT_EQ(ap.rssi, -52);
}

DMOD_TEST_STEP(rpc_reads_events)
{
    rpc_message_t        m;
    dmesphosted_status_t st;
    memset(&st, 0, sizeof(st));

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_event_connected, sizeof(fx_event_connected), &m));
    DMOD_TEST_EXPECT_EQ(m.type, RPC_TYPE_EVENT);
    DMOD_TEST_EXPECT_EQ(m.msg_id, RPC_EVENT_STA_CONNECTED);
    DMOD_TEST_EXPECT_TRUE(rpc_read_sta_connected(&m, &st));
    DMOD_TEST_EXPECT_EQ(strcmp(st.ssid, "Home Network"), 0);
    DMOD_TEST_EXPECT_EQ(st.bssid[0], 0xF4);
    DMOD_TEST_EXPECT_EQ(st.channel, 6);

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_event_disconnected, sizeof(fx_event_disconnected), &m));
    DMOD_TEST_EXPECT_EQ(m.msg_id, RPC_EVENT_STA_DISCONNECTED);
    DMOD_TEST_EXPECT_EQ(rpc_read_sta_disconnected(&m), 201);

    DMOD_TEST_EXPECT_TRUE(rpc_decode(fx_event_scan_done, sizeof(fx_event_scan_done), &m));
    DMOD_TEST_EXPECT_EQ(m.msg_id, RPC_EVENT_STA_SCAN_DONE);
}

DMOD_TEST_STEP(rpc_wraps_and_unwraps)
{
    uint8_t        out[64];
    char           ep[RPC_EP_NAME_MAX];
    const uint8_t *pb;
    size_t         pb_len;

    size_t n = rpc_wrap(out, sizeof(out), RPC_EP_REQUEST, fx_req_set_mode, sizeof(fx_req_set_mode));
    DMOD_TEST_EXPECT_EQ(n, 3 + 6 + 3 + sizeof(fx_req_set_mode));
    DMOD_TEST_EXPECT_EQ(out[0], RPC_TLV_EP_NAME);
    DMOD_TEST_EXPECT_EQ(out[1], 6);
    DMOD_TEST_EXPECT_EQ(memcmp(out + 3, "RPCRsp", 6), 0);
    DMOD_TEST_EXPECT_EQ(out[9], RPC_TLV_DATA);
    DMOD_TEST_EXPECT_EQ(out[10], sizeof(fx_req_set_mode));

    DMOD_TEST_EXPECT_TRUE(rpc_unwrap(out, n, ep, &pb, &pb_len));
    DMOD_TEST_EXPECT_EQ(strcmp(ep, "RPCRsp"), 0);
    DMOD_TEST_EXPECT_EQ(pb_len, sizeof(fx_req_set_mode));
    DMOD_TEST_EXPECT_EQ(memcmp(pb, fx_req_set_mode, pb_len), 0);

    DMOD_TEST_EXPECT_FALSE(rpc_unwrap(out, n - 1, ep, &pb, &pb_len));     /* The data cut short */
    DMOD_TEST_EXPECT_FALSE(rpc_unwrap(out, 9, ep, &pb, &pb_len));         /* No data */
    DMOD_TEST_EXPECT_EQ(rpc_wrap(out, 8, RPC_EP_REQUEST, fx_req_set_mode, sizeof(fx_req_set_mode)), 0);
}

/* ---- The bring-up ---- */

/* The init event of coprocessor/'s firmware (TLV v2): chip, capabilities,
 * raw throughput, queue sizes, version 3.0.9, extended capabilities, RPC 2 */
static const uint8_t g_init_event[] = {
    0x22, 0x1E,
    0x12, 0x01, 0x05,
    0x11, 0x01, 0xA0,
    0x13, 0x01, 0x00,
    0x14, 0x01, 0x14,
    0x15, 0x01, 0x14,
    0x17, 0x04, 0x09, 0x00, 0x03, 0x00,
    0x16, 0x04, 0x00, 0x00, 0x00, 0x00,
    0x1A, 0x01, 0x02,
};

DMOD_TEST_STEP(priv_reads_the_init_event_and_answers)
{
    priv_init_t init;
    DMOD_TEST_EXPECT_TRUE(priv_parse_init(g_init_event, sizeof(g_init_event), &init));
    DMOD_TEST_EXPECT_EQ(init.chip_id, DMESPHOSTED_CHIP_ESP32C3);
    DMOD_TEST_EXPECT_EQ(init.capabilities, PRIV_CAP_WLAN_SPI | PRIV_CAP_CHECKSUM);
    DMOD_TEST_EXPECT_EQ(init.firmware_version, 0x030009);
    DMOD_TEST_EXPECT_EQ(init.ext_capabilities, 0);
    DMOD_TEST_EXPECT_TRUE(init.has_rpc_version);
    DMOD_TEST_EXPECT_EQ(init.rpc_version, PRIV_RPC_VERSION);

    /* What Espressif's MCU host answers (eh_host_transport_build_host_caps_pkt) */
    static const uint8_t expected[] = {
        0x22, 0x12,
        0x44, 0x01, 0x00,
        0x45, 0x01, 0x05,
        0x46, 0x01, 0x00,
        0x47, 0x01, 0x00,
        0x48, 0x01, 0x00,
        0x1A, 0x01, 0x02,
    };
    uint8_t out[PRIV_HOST_CONFIG_MAX];
    DMOD_TEST_EXPECT_EQ(priv_build_host_config(out, &init), sizeof(expected));
    DMOD_TEST_EXPECT_EQ(memcmp(out, expected, sizeof(expected)), 0);

    /* No RPC version from it: none back (an older one may not skip it) */
    init.has_rpc_version = false;
    DMOD_TEST_EXPECT_EQ(priv_build_host_config(out, &init), sizeof(expected) - 3);
    DMOD_TEST_EXPECT_EQ(out[1], 0x0F);
}

DMOD_TEST_STEP(priv_rejects_what_is_not_an_init_event)
{
    priv_init_t init;
    uint8_t     bad[sizeof(g_init_event)];

    memcpy(bad, g_init_event, sizeof(bad));
    bad[0] = 0x00;
    DMOD_TEST_EXPECT_FALSE(priv_parse_init(bad, sizeof(bad), &init));

    memcpy(bad, g_init_event, sizeof(bad));
    bad[1] = 0x40;          /* Longer than it is */
    DMOD_TEST_EXPECT_FALSE(priv_parse_init(bad, sizeof(bad), &init));

    memcpy(bad, g_init_event, sizeof(bad));
    bad[3] = 0x30;          /* A TLV past the end */
    DMOD_TEST_EXPECT_FALSE(priv_parse_init(bad, sizeof(bad), &init));
}

/* ---- Names ---- */

DMOD_TEST_STEP(names_of_states_and_chips)
{
    DMOD_TEST_EXPECT_EQ(strcmp(dmesphosted_state_name(dmesphosted_state_connected), "connected"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(dmesphosted_chip_name(DMESPHOSTED_CHIP_ESP32C3), "ESP32-C3"), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(dmesphosted_chip_name(0x42), "an ESP"), 0);
}

/* ---- The driver, through the dmdrvi DIF, the way dmdevfs calls it ---- */

/* No SPI bus and no pins on the host: they are never reported as friends,
 * so the co-processor is never reached - the device is there, off. */
#define TEST_INI \
    "[wlan0]\n" \
    "driver_name=dmesphosted\n"

typedef struct
{
    dmod_dmdrvi_create_t    create;
    dmod_dmdrvi_free_t      free;
    dmod_dmdrvi_open_t      open;
    dmod_dmdrvi_close_t     close;
    dmod_dmdrvi_read_t      read;
    dmod_dmdrvi_write_t     write;
    dmod_dmdrvi_ioctl_t     ioctl;
} driver_t;

typedef struct
{
    driver_t            drv;
    dmini_context_t     ini;
    dmdrvi_context_t    ctx;
    dmdrvi_dev_num_t    dev_num;
    void*               handle;
} device_t;

static bool get_driver(driver_t* drv)
{
    Dmod_Context_t* module = Dmod_GetModuleContext("dmesphosted");
    if (module == NULL)
        return false;
    drv->create = Dmod_GetDifFunction(module, dmod_dmdrvi_create_sig);
    drv->free   = Dmod_GetDifFunction(module, dmod_dmdrvi_free_sig);
    drv->open   = Dmod_GetDifFunction(module, dmod_dmdrvi_open_sig);
    drv->close  = Dmod_GetDifFunction(module, dmod_dmdrvi_close_sig);
    drv->read   = Dmod_GetDifFunction(module, dmod_dmdrvi_read_sig);
    drv->write  = Dmod_GetDifFunction(module, dmod_dmdrvi_write_sig);
    drv->ioctl  = Dmod_GetDifFunction(module, dmod_dmdrvi_ioctl_sig);
    return drv->create != NULL && drv->free != NULL && drv->open != NULL && drv->close != NULL &&
           drv->read != NULL && drv->write != NULL && drv->ioctl != NULL;
}

static bool device_create(device_t* dev, const char* ini_text, const char* section)
{
    memset(dev, 0, sizeof(*dev));
    if (!get_driver(&dev->drv))
        return false;
    dev->ini = dmini_create();
    dmini_parse_string(dev->ini, ini_text);
    dmini_set_active_section(dev->ini, section, 0);
    dev->ctx = dev->drv.create(dev->ini, &dev->dev_num);
    return dev->ctx != NULL;
}

static bool device_open(device_t* dev, const char* ini_text)
{
    if (!device_create(dev, ini_text, "wlan0"))
        return false;
    dev->handle = dev->drv.open(dev->ctx, DMDRVI_O_RDWR, &dev->dev_num);
    return dev->handle != NULL;
}

static void device_close(device_t* dev)
{
    if (dev->handle != NULL)
        dev->drv.close(dev->ctx, dev->handle);
    if (dev->ctx != NULL)
        dev->drv.free(dev->ctx);
    if (dev->ini != NULL)
        dmini_destroy(dev->ini);
}

DMOD_TEST_STEP(driver_is_off_without_a_co_processor)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));
    DMOD_TEST_EXPECT_EQ(dev.dev_num.flags, DMDRVI_NUM_ALT_NAME);
    DMOD_TEST_EXPECT_EQ(strcmp(dev.dev_num.alt_name, "wlan0"), 0);

    dmesphosted_info_t info;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_get_info, &info), 0);
    DMOD_TEST_EXPECT_EQ(info.chip_id, DMESPHOSTED_CHIP_UNKNOWN);

    dmesphosted_status_t st;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_get_status, &st), 0);
    DMOD_TEST_EXPECT_EQ(st.state, dmesphosted_state_off);

    dmdrvi_net_link_status_t link = DMDRVI_NET_LINK_UP;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_GET_LINK_STATUS, &link), 0);
    DMOD_TEST_EXPECT_EQ(link, DMDRVI_NET_LINK_DOWN);

    dmdrvi_net_mac_addr_t mac;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_GET_MAC_ADDR, &mac), -ENODEV);
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_SET_MAC_ADDR, &mac), -ENOTSUP);

    dmesphosted_ap_t   aps[2];
    dmesphosted_scan_t scan = { aps, 2, 0 };
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_scan, &scan), -ENODEV);
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, 0x7777, NULL), -ENOTTY);

    uint8_t frame[64] = { 0 };
    DMOD_TEST_EXPECT_EQ(dev.drv.read(dev.ctx, dev.handle, frame, sizeof(frame), 0), 0);    /* Not started */
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_START, NULL), 0);
    DMOD_TEST_EXPECT_EQ(dev.drv.write(dev.ctx, dev.handle, frame, sizeof(frame), 0), 0);   /* Not connected */
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, DMDRVI_IOCTL_NET_STOP, NULL), 0);
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_get_info, &info), 0);
    DMOD_TEST_EXPECT_EQ(info.tx_dropped, 1);

    device_close(&dev);
}

DMOD_TEST_STEP(driver_keeps_the_network_to_connect_to)
{
    device_t dev;
    DMOD_TEST_EXPECT_TRUE(device_open(&dev, TEST_INI));

    dmesphosted_connect_t net;
    memset(&net, 0, sizeof(net));
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_connect, &net), -EINVAL);
    strcpy(net.ssid, "Home Network");
    strcpy(net.password, "short");
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_connect, &net), -EINVAL);
    strcpy(net.password, "secret123");
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_connect, &net), 0);

    dmesphosted_status_t st;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_get_status, &st), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(st.ssid, "Home Network"), 0);
    DMOD_TEST_EXPECT_EQ(st.state, dmesphosted_state_off);

    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_disconnect, NULL), 0);
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, dev.handle, dmesphosted_ioctl_cmd_get_status, &st), 0);
    DMOD_TEST_EXPECT_EQ(st.ssid[0], '\0');

    device_close(&dev);
}

DMOD_TEST_STEP(driver_checks_its_configuration)
{
    device_t dev;
    DMOD_TEST_EXPECT_FALSE(device_create(&dev, "[w]\ndriver_name=dmesphosted\npoll_interval_ms=0\n", "w"));
    device_close(&dev);
    DMOD_TEST_EXPECT_FALSE(device_create(&dev, "[w]\ndriver_name=dmesphosted\ntx_power=30\n", "w"));
    device_close(&dev);
    DMOD_TEST_EXPECT_FALSE(device_create(&dev, "[w]\ndriver_name=dmesphosted\npower_save=sometimes\n", "w"));
    device_close(&dev);
    DMOD_TEST_EXPECT_FALSE(device_create(&dev,
        "[w]\ndriver_name=dmesphosted\nssid=0123456789012345678901234567890123456789\n", "w"));
    device_close(&dev);

    DMOD_TEST_EXPECT_TRUE(device_create(&dev, "[w]\ndriver_name=dmesphosted\nssid=Cafe\ntx_power=8\n", "w"));
    dmesphosted_status_t st;
    DMOD_TEST_EXPECT_EQ(dev.drv.ioctl(dev.ctx, NULL, dmesphosted_ioctl_cmd_get_status, &st), 0);
    DMOD_TEST_EXPECT_EQ(strcmp(st.ssid, "Cafe"), 0);
    device_close(&dev);
}

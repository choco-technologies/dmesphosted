#ifndef DMESPHOSTED_RPC_MSG_H
#define DMESPHOSTED_RPC_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pb.h"
#include "dmesphosted_types.h"

/*
 * ESP-Hosted's RPC (the MCU one, RPC version 2 - rpc_v2.proto of
 * esp-hosted-mcu): a protobuf message Rpc { msg_type = 1; msg_id = 2;
 * uid = 3; oneof payload } whose payload is the field numbered as msg_id.
 * A request's response is msg_id + RPC_RESP_OFFSET; events are 0x300 on.
 *
 * On the bus a message is wrapped in protocomm's TLVs
 *   [0x01][len:2 LE][endpoint name][0x02][len:2 LE][protobuf]
 * and sent on FRAME_IF_SERIAL, in fragments when longer than a frame.
 */

#define RPC_TYPE_REQ                    1u
#define RPC_TYPE_RESP                   2u
#define RPC_TYPE_EVENT                  3u

#define RPC_RESP_OFFSET                 0x100u

/* Requests (RpcId) */
#define RPC_REQ_SET_WIFI_MODE           0x104u
#define RPC_REQ_WIFI_SET_PS             0x10Eu
#define RPC_REQ_WIFI_SET_MAX_TX_POWER   0x113u
#define RPC_REQ_WIFI_INIT               0x116u
#define RPC_REQ_WIFI_START              0x118u
#define RPC_REQ_WIFI_CONNECT            0x11Au
#define RPC_REQ_WIFI_DISCONNECT         0x11Bu
#define RPC_REQ_WIFI_SET_CONFIG         0x11Cu
#define RPC_REQ_WIFI_SCAN_START         0x11Eu
#define RPC_REQ_WIFI_SCAN_GET_AP_NUM    0x120u
#define RPC_REQ_WIFI_SCAN_GET_AP_RECORDS 0x121u
#define RPC_REQ_WIFI_STA_GET_AP_INFO    0x126u
#define RPC_REQ_IFACE_MAC_ADDR_SET_GET  0x181u

/* Events */
#define RPC_EVENT_ESP_INIT              0x301u
#define RPC_EVENT_HEARTBEAT             0x302u
#define RPC_EVENT_WIFI_EVENT_NO_ARGS    0x305u
#define RPC_EVENT_STA_SCAN_DONE         0x306u
#define RPC_EVENT_STA_CONNECTED         0x307u
#define RPC_EVENT_STA_DISCONNECTED      0x308u

/* esp_wifi's values the requests carry */
#define RPC_WIFI_IF_STA                 0u
#define RPC_WIFI_MODE_STA               1u
#define RPC_WIFI_PS_NONE                0u      /* wifi_ps_type_t */
#define RPC_WIFI_PS_MIN_MODEM           1u
#define RPC_WIFI_PS_MAX_MODEM           2u

/* The endpoint requests go to and events come from (RPC version 2) */
#define RPC_EP_REQUEST                  "RPCRsp"
#define RPC_EP_EVENT                    "RPCEvt"
#define RPC_EP_NAME_MAX                 16u

#define RPC_TLV_EP_NAME                 0x01u
#define RPC_TLV_DATA                    0x02u

/* ---- Writing a request ---- */

/* Starts Rpc { Req, msg_id, uid, payload = msg_id { ... */
void rpc_begin_request(pb_writer_t *w, uint32_t msg_id, uint32_t uid);
/* ... } }: the bytes of the Rpc message, 0 when it did not fit */
size_t rpc_end_request(pb_writer_t *w);

/* The payloads (between rpc_begin_request() and rpc_end_request()) */
void rpc_put_wifi_init(pb_writer_t *w);
void rpc_put_sta_config(pb_writer_t *w, const char *ssid, const char *password);

/**
 * Wraps the Rpc message @p pb in protocomm's TLVs for @p endpoint into
 * @p out; returns the bytes, 0 when they do not fit.
 */
size_t rpc_wrap(uint8_t *out, size_t size, const char *endpoint, const uint8_t *pb, size_t pb_len);

/**
 * Reads protocomm's TLVs: the endpoint (copied, NUL ended) and the protobuf
 * (pointing into @p in). False when they are not there.
 */
bool rpc_unwrap(const uint8_t *in, size_t len, char endpoint[RPC_EP_NAME_MAX],
                const uint8_t **pb, size_t *pb_len);

/* ---- Reading a response or an event ---- */

typedef struct
{
    uint32_t        type;           /* RPC_TYPE_* */
    uint32_t        msg_id;
    uint32_t        uid;
    const uint8_t  *payload;        /* The field msg_id's content (NULL: none) */
    size_t          payload_len;
} rpc_message_t;

bool rpc_decode(const uint8_t *pb, size_t len, rpc_message_t *msg);

/* Every response's and event's field 1: esp_err_t of the call (0 = ESP_OK) */
int32_t rpc_resp_code(const rpc_message_t *msg);

/* An int32 field of the payload (0 when it is not there) */
int32_t rpc_payload_int(const rpc_message_t *msg, uint32_t field);

/* Rpc_Resp_IfaceMacAddrSetGet's mac */
bool rpc_read_mac(const rpc_message_t *msg, uint8_t mac[DMESPHOSTED_MAC_LEN]);

/* Rpc_Event_StaConnected: the network and its access point */
bool rpc_read_sta_connected(const rpc_message_t *msg, dmesphosted_status_t *status);

/* Rpc_Event_StaDisconnected: the reason (wifi_err_reason_t) */
uint16_t rpc_read_sta_disconnected(const rpc_message_t *msg);

/* Rpc_Resp_WifiScanGetApRecords' records, at most @p max; returns how many */
uint16_t rpc_read_ap_records(const rpc_message_t *msg, dmesphosted_ap_t *aps, uint16_t max);

/* Rpc_Resp_WifiStaGetApInfo's record */
bool rpc_read_ap_info(const rpc_message_t *msg, dmesphosted_ap_t *ap);

#endif /* DMESPHOSTED_RPC_MSG_H */

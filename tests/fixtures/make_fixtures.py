#!/usr/bin/env python3
"""
Makes tests/rpc_fixtures.h: RPC messages encoded by Google's protobuf from
ESP-Hosted's own rpc_v2.proto - what the driver's requests must be byte for
byte, and responses / events as the co-processor sends them for the driver
to read. Run again when the protocol (ESP-Hosted's version) changes:

    pip install grpcio-tools protobuf
    python3 tests/fixtures/make_fixtures.py \\
        [coprocessor/build/esp_hosted/common/serializers/eh_proto_v2/src/proto/rpc_v2.proto]

(coprocessor/build.sh clones esp-hosted-mcu there.)
"""
import importlib
import os
import sys
import tempfile

from grpc_tools import protoc

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
PROTO = os.path.join(REPO, "coprocessor/build/esp_hosted/common/serializers/eh_proto_v2/src/proto/rpc_v2.proto")
OUT = os.path.join(REPO, "tests/rpc_fixtures.h")

MAC = bytes.fromhex("9ccc01a81e68")
BSSID = bytes.fromhex("f4ec38112233")


def load(proto):
    out = tempfile.mkdtemp()
    if protoc.main(["protoc", f"-I{os.path.dirname(proto)}", f"--python_out={out}", proto]) != 0:
        sys.exit("protoc failed")
    sys.path.insert(0, out)
    return importlib.import_module(os.path.splitext(os.path.basename(proto))[0] + "_pb2")


def main():
    proto = sys.argv[1] if len(sys.argv) > 1 else PROTO
    pb = load(proto)
    fixtures = []

    def rpc(name, comment, msg_type, msg_id, uid, field, fill=None):
        m = pb.Rpc(msg_type=msg_type, msg_id=msg_id, uid=uid)
        sub = getattr(m, field)
        sub.SetInParent()
        if fill:
            fill(sub)
        fixtures.append((name, comment, m.SerializeToString()))

    Req, Resp, Event = pb.Req, pb.Resp, pb.Event
    ids = pb.RpcId

    # ---- Requests: what the driver writes ----
    rpc("fx_req_get_mac", "IfaceMacAddrSetGet { set = false, type = STA }",
        Req, ids.Value("Req_IfaceMacAddrSetGet"), 1, "req_iface_mac_addr_set_get")

    def init_cfg(r):
        c = r.cfg
        c.static_rx_buf_num, c.dynamic_rx_buf_num, c.tx_buf_type = 10, 32, 1
        c.dynamic_tx_buf_num, c.ampdu_rx_enable, c.ampdu_tx_enable = 32, 1, 1
        c.nvs_enable, c.rx_ba_win, c.beacon_max_len, c.mgmt_sbuf_num = 1, 6, 752, 32
        c.feature_caps, c.sta_disconnected_pm, c.espnow_max_encrypt_num = 1, True, 7
        c.magic, c.rx_mgmt_buf_num, c.tx_hetb_queue_num = 0x1F2F3F4F, 5, 3
    rpc("fx_req_wifi_init", "WifiInit { the default config }",
        Req, ids.Value("Req_WifiInit"), 2, "req_wifi_init", init_cfg)

    rpc("fx_req_set_mode", "SetWifiMode { mode = STA }",
        Req, ids.Value("Req_SetWifiMode"), 3, "req_set_wifi_mode", lambda r: setattr(r, "mode", 1))
    rpc("fx_req_wifi_start", "WifiStart { }",
        Req, ids.Value("Req_WifiStart"), 4, "req_wifi_start")

    def sta(r):
        r.iface = 0
        r.cfg.sta.ssid = b"Home Network"
        r.cfg.sta.password = b"secret123"
        r.cfg.sta.pmf_cfg.capable = True
    rpc("fx_req_set_config", "WifiSetConfig { STA, \"Home Network\" / \"secret123\", PMF capable }",
        Req, ids.Value("Req_WifiSetConfig"), 5, "req_wifi_set_config", sta)

    def sta_open(r):
        r.cfg.sta.ssid = b"Cafe"
        r.cfg.sta.pmf_cfg.capable = True
    rpc("fx_req_set_config_open", "WifiSetConfig { STA, \"Cafe\", open }",
        Req, ids.Value("Req_WifiSetConfig"), 6, "req_wifi_set_config", sta_open)

    rpc("fx_req_scan_start", "WifiScanStart { }",
        Req, ids.Value("Req_WifiScanStart"), 7, "req_wifi_scan_start")
    rpc("fx_req_get_ap_records", "WifiScanGetApRecords { number = 16 }",
        Req, ids.Value("Req_WifiScanGetApRecords"), 8, "req_wifi_scan_get_ap_records",
        lambda r: setattr(r, "number", 16))
    rpc("fx_req_set_ps_none", "SetPs { type = NONE }",
        Req, ids.Value("Req_WifiSetPs"), 10, "req_wifi_set_ps")
    rpc("fx_req_set_ps_min", "SetPs { type = MIN_MODEM }",
        Req, ids.Value("Req_WifiSetPs"), 11, "req_wifi_set_ps", lambda r: setattr(r, "type", 1))
    rpc("fx_req_set_tx_power", "WifiSetMaxTxPower { power = 80 (20 dBm) }",
        Req, ids.Value("Req_WifiSetMaxTxPower"), 9, "req_set_wifi_max_tx_power",
        lambda r: setattr(r, "power", 80))

    # ---- Responses and events: what the co-processor sends ----
    def mac(r):
        r.resp, r.set, r.type, r.mac = 0, False, 0, MAC
    rpc("fx_resp_get_mac", "Resp IfaceMacAddrSetGet { mac = 9c:cc:01:a8:1e:68 }",
        Resp, ids.Value("Resp_IfaceMacAddrSetGet"), 1, "resp_iface_mac_addr_set_get", mac)
    rpc("fx_resp_wifi_init_error", "Resp WifiInit { resp = 0x3001 (ESP_ERR_WIFI_NOT_INIT) }",
        Resp, ids.Value("Resp_WifiInit"), 2, "resp_wifi_init", lambda r: setattr(r, "resp", 0x3001))
    rpc("fx_resp_ap_num", "Resp WifiScanGetApNum { number = 3 }",
        Resp, ids.Value("Resp_WifiScanGetApNum"), 7, "resp_wifi_scan_get_ap_num",
        lambda r: setattr(r, "number", 3))

    def records(r):
        r.number = 2
        a = r.ap_records.add()
        a.bssid, a.ssid, a.primary, a.second, a.rssi, a.authmode = BSSID, b"Home Network", 6, 0, -47, 3
        a.pairwise_cipher, a.group_cipher, a.bitmask = 4, 4, 0x1F
        a.country.cc, a.country.schan, a.country.nchan, a.country.max_tx_power = b"PL\x00", 1, 13, 20
        b = r.ap_records.add()
        b.bssid, b.ssid, b.primary, b.rssi, b.authmode = bytes(6), b"Cafe", 11, -80, 0
    rpc("fx_resp_ap_records", "Resp WifiScanGetApRecords { \"Home Network\" -47 dBm ch 6 WPA2, \"Cafe\" -80 ch 11 open }",
        Resp, ids.Value("Resp_WifiScanGetApRecords"), 8, "resp_wifi_scan_get_ap_records", records)

    def ap_info(r):
        r.ap_record.bssid, r.ap_record.ssid, r.ap_record.primary, r.ap_record.rssi = BSSID, b"Home Network", 6, -52
    rpc("fx_resp_ap_info", "Resp WifiStaGetApInfo { -52 dBm }",
        Resp, ids.Value("Resp_WifiStaGetApInfo"), 10, "resp_wifi_sta_get_ap_info", ap_info)

    def connected(e):
        s = e.sta_connected
        s.ssid, s.ssid_len, s.bssid, s.channel, s.authmode, s.aid = b"Home Network", 12, BSSID, 6, 3, 1
    rpc("fx_event_connected", "Event StaConnected { \"Home Network\", channel 6 }",
        Event, ids.Value("Event_StaConnected"), 0, "event_sta_connected", connected)

    def disconnected(e):
        s = e.sta_disconnected
        s.ssid, s.ssid_len, s.bssid, s.reason, s.rssi = b"Home Network", 12, BSSID, 201, -90
    rpc("fx_event_disconnected", "Event StaDisconnected { reason 201 (NO_AP_FOUND) }",
        Event, ids.Value("Event_StaDisconnected"), 0, "event_sta_disconnected", disconnected)

    def scan_done(e):
        e.scan_done.status, e.scan_done.number = 0, 2
    rpc("fx_event_scan_done", "Event StaScanDone { 2 found }",
        Event, ids.Value("Event_StaScanDone"), 0, "event_sta_scan_done", scan_done)

    with open(OUT, "w") as f:
        f.write("/* Made by tests/fixtures/make_fixtures.py from ESP-Hosted's rpc_v2.proto\n"
                " * (Google's protobuf) - do not edit, run it again. */\n"
                "#ifndef RPC_FIXTURES_H\n#define RPC_FIXTURES_H\n\n#include <stdint.h>\n\n")
        for name, comment, data in fixtures:
            body = ", ".join(f"0x{b:02x}" for b in data)
            lines = []
            while body:
                cut = body[:96].rfind(", ") + 2 if len(body) > 96 else len(body)
                lines.append("    " + body[:cut].strip())
                body = body[cut:]
            f.write(f"/* {comment} */\nstatic const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n\n")
        f.write("#endif /* RPC_FIXTURES_H */\n")
    print(f"{OUT}: {len(fixtures)} messages")


if __name__ == "__main__":
    main()

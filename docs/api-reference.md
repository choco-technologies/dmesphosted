# dmesphosted - API reference

## The node

A network device like dmeth's:

| | |
|---|---|
| `read()` | One received 802.3 frame (truncated to the buffer); 0 when none came within 1 s, or the interface is not started |
| `write()` | One 802.3 frame to send (at most 1588 bytes); 0 when it cannot go - not connected, not started, no room within 100 ms |
| `DMDRVI_IOCTL_NET_GET_MAC_ADDR` | The co-processor's station MAC; `-ENODEV` until it is reached |
| `DMDRVI_IOCTL_NET_SET_MAC_ADDR` | `-ENOTSUP` (the co-processor's own) |
| `DMDRVI_IOCTL_NET_GET_LINK_STATUS` | Up while connected |
| `DMDRVI_IOCTL_NET_START` / `_STOP` | Frames go / do not (dmnetif's up / down) |

The driver registers the interface with dmnetif itself
(`dmnetif_register(interface, node)`).

## The Wi-Fi - `dmesphosted_types.h`

| Command | `arg` | |
|---|---|---|
| `dmesphosted_ioctl_cmd_get_info` | `dmesphosted_info_t*` | The co-processor: chip, firmware version, capabilities, MAC; frame counters, restarts |
| `dmesphosted_ioctl_cmd_get_status` | `dmesphosted_status_t*` | The state, the network set, its access point, channel and signal (asked when connected) and the last disconnection's reason |
| `dmesphosted_ioctl_cmd_connect` | `const dmesphosted_connect_t*` | The network to connect to (SSID 1..32, password empty or 8..64). Kept: connected again whenever lost. Returns at once - the status says when |
| `dmesphosted_ioctl_cmd_disconnect` | - | Disconnects and forgets the network |
| `dmesphosted_ioctl_cmd_scan` | `dmesphosted_scan_t*` | The networks in range (at most `max`, at most 16): waits for the scan, a few seconds. `-ENODEV` before its Wi-Fi started, `-EBUSY` while it connects |

Errors: `-ENODEV` no co-processor, `-ETIMEDOUT` no answer, `-EIO` it
answered an error (the log has its `esp_err_t`), `-EINVAL`.

`reason` is esp_wifi's `wifi_err_reason_t`: 15 / 204 a handshake failed (a
wrong password), 201 no such network, 202 / 203 / 205 refused by the access
point, 210 / 211 none with a security it takes.

## Functions

| | |
|---|---|
| `dmesphosted_state_name(state)` | "connected", ... |
| `dmesphosted_chip_name(chip_id)` | "ESP32-C3", ... |

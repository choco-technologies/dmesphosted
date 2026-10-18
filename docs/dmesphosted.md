# dmesphosted - how it works

## The parts

| File | What |
|------|------|
| `src/dmesphosted.c` | The dmdrvi driver: configuration, the node, `read`/`write`/`ioctl`, friends, dmnetif |
| `src/bus.c` | The SPI transport and its thread - the bus's only user |
| `src/frame.c` | The frame header (encode / decode / checksum) |
| `src/priv.c` | The bring-up: the co-processor's init event, the host's answer |
| `src/rpc.c` | RPC calls: one at a time, waiting for their response |
| `src/rpc_msg.c`, `src/pb.c` | The RPC messages, and as much of protobuf as they need |
| `src/wifi.c` | The Wi-Fi: the control thread, events, connect / scan / status |
| `src/queue.c` | Fixed-size frame queues |

Two threads, in a process of the driver's own: the **bus thread** (every SPI
transaction, the pins, the co-processor's restarts) and the **control
thread** (the RPC calls that start the Wi-Fi and connect). An `ioctl` (scan,
connect) makes its RPC calls in its caller's thread; `read`/`write` only
queue frames.

## The bus

Every SPI transaction is 1600 bytes both ways (`FRAME_BUFFER_SIZE`): what
the host has to send - or an empty frame - out, the co-processor's frame -
or its empty one - in. The co-processor's two pins:

- **Handshake** - it has a transaction prepared and can be clocked. The host
  clocks only then: clocking without it loses the frame.
- **Data Ready** - what it prepared is a real frame.

The bus thread clocks when Handshake is high and either the host has
something to send or Data Ready is high; otherwise it waits for a pin's
interrupt (dmgpio's `interrupt_handler` → dmhaman → a semaphore), looking at
the levels anyway every 100 ms. Without a Data Ready pin it polls every
`poll_interval_ms`.

Frames to send are queued: PRIV and SERIAL (RPC) first, then 802.3 frames
(when connected, and not throttled by the co-processor).

## A frame

```
 0  if_type:4 | if_num:4    1 STA (802.3), 3 SERIAL (RPC), 5 PRIV (bring-up), 8 nothing
 1  flags                   0x01: more fragments follow (SERIAL)
 2  len (2, LE)             the payload's bytes
 4  offset (2)              12
 6  checksum (2)            byte sum of header and payload, this field 0
 8  seq_num (2)
10  throttle:2              the co-processor's flow control
11  priv_pkt_type           0x33 (event) on PRIV
12  payload...
```

The co-processor advertises whether it checks checksums (capability bit
0x80); `coprocessor/`'s firmware does, and so does the driver.

## The bring-up

Whenever the co-processor is to be reached - at the start, when its nodes
change, when it stopped answering - the bus thread restarts it (the reset
pin, LOW 5 ms) and waits up to 3 s for its first frame, the init event:

```
PRIV  [0x22][len]  [0x12 1 chip][0x11 1 caps][0x13 1 ..][0x14 1 ..][0x15 1 ..]
                   [0x17 4 version][0x16 4 ext caps][0x1A 1 RPC version]
```

and answers what Espressif's MCU host answers:

```
PRIV  [0x22][len]  [0x44 1 0][0x45 1 chip][0x46 1 0][0x47 1 0][0x48 1 0][0x1A 1 2]
```

(no host capabilities, the chip seen, no throughput test, no flow control,
RPC version 2 back - the co-processor aborts on a mismatch, so a co-processor
speaking another version is not answered). Then RPC and the data path go.

No init event within 3 s: tried again every 2 s (said once in the log).

## RPC

ESP-Hosted's MCU RPC (version 2, esp-hosted-mcu's `rpc_v2.proto`):
protobuf `Rpc { msg_type = 1; msg_id = 2; uid = 3; oneof payload }`, the
payload's field number being `msg_id`; a request's response is
`msg_id + 0x100`, events are `0x300` on. On the bus a message is wrapped in
protocomm's TLVs and sent on SERIAL, in fragments when over a frame:

```
[0x01][len:2 LE]["RPCRsp"][0x02][len:2 LE][protobuf]
```

The requests are esp_wifi's API: `IfaceMacAddrSetGet`, `WifiInit`,
`SetWifiMode`, `WifiStart`, `WifiSetConfig`, `WifiConnect`,
`WifiDisconnect`, `WifiScanStart`, `WifiScanGetApNum`,
`WifiScanGetApRecords`, `WifiStaGetApInfo`, `WifiSetMaxTxPower`; the
events: `StaConnected`, `StaDisconnected`, `StaScanDone`.

The protobuf is written and read by `pb.c` - varints, length-delimited
fields, messages in messages - not by generated code: the tests check every
request byte for byte against Google's protobuf from `rpc_v2.proto`
(`tests/fixtures/make_fixtures.py` → `tests/rpc_fixtures.h`), and read
responses and events it made.

## The Wi-Fi

Once the co-processor is up the control thread starts its Wi-Fi as a
station: the MAC, `WifiInit` (an error there: it was initialized already -
goes on), `SetWifiMode(STA)`, `WifiStart`, the transmit power if
configured. Three failures: the co-processor is restarted.

With a network set (`ssid=` in the configuration, or
`dmesphosted_ioctl_cmd_connect`): `WifiSetConfig` + `WifiConnect`. Its
events then say: `StaConnected` - the link is up (`DMDRVI_IOCTL_NET_GET_LINK_STATUS`;
dmnetbridge brings the interface up, DHCP follows); `StaDisconnected` -
down, and connected again after 1 s, 2 s, 4 s ... up to 30 s. No event
within 20 s of connecting: tried again.

## States

| `dmesphosted_state_t` | |
|---|---|
| `off` | No co-processor: not reached (yet), or lost |
| `starting` | Reached; its Wi-Fi starting |
| `idle` | Its Wi-Fi started; not connected |
| `connecting` | |
| `connected` | The link is up |

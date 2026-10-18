# dmesphosted

Wi-Fi for a [DMOD](https://github.com/choco-technologies/dmod) system, on an
Espressif chip used as a network card: the chip runs Espressif's
[ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) co-processor
firmware, the host talks to it over SPI. The chip passes 802.3 frames and
takes RPC calls (scan, connect) - the TCP/IP stack stays the host's own
(dmnetif, dmip, dmdhcp, dmdns, ...): `wlan0` is an interface like `eth0`.

```
┌───────────────────────────────────────────┐
│  dmip / dmudp / dmtcp / dhcpc / ifconfig  │  the host's stack, unchanged
├───────────────────────────────────────────┤
│                  DMNETIF                  │  "wlan0"
├───────────────────────────────────────────┤
│              DMESPHOSTED                  │  this module
│  frames <-> 802.3, RPC (Wi-Fi), bring-up  │
├───────────────────────────────────────────┤
│   dmspi (SPI2)  +  dmgpio (Handshake,     │
│   Data Ready, Reset - via dmhaman IRQs)   │
└──────────────────────┬────────────────────┘
                       │ SPI, 8 wires
┌──────────────────────┴────────────────────┐
│  ESP32-C3: ESP-Hosted co-processor        │  coprocessor/
│  (esp_wifi, the radio)                    │
└───────────────────────────────────────────┘
```

The driver is written for the protocol, not for a board: anything that
speaks ESP-Hosted's MCU protocol (RPC version 2) over SPI is a dmesphosted
device. What the ESP runs, and how to put it there, is in
[`coprocessor/`](coprocessor/README.md).

## What is where

| | |
|---|---|
| [`coprocessor/`](coprocessor/README.md) | The ESP's firmware: its configuration per board, `build.sh`, `flash.sh`, `check.py`, **how to prepare a board** |
| [`configs/board/`](configs/board/stm32f746g-disco/README.md) | The host's side: `wifi.ini` per board, **the wires** |
| `src/`, `include/` | The driver |
| [`docs/`](docs/dmesphosted.md) | How it works, the configuration, the API |
| `tests/` | Host tests (`dmod_loader`): frames, protobuf, RPC against Google's protobuf, the bring-up, the driver |

## Using it

1. Prepare the ESP: [`coprocessor/README.md`](coprocessor/README.md) - a
   release's image, `flash.sh`, a few minutes.
2. Wire it: e.g. [`configs/board/stm32f746g-disco`](configs/board/stm32f746g-disco/README.md).
3. Install it with the board's configuration:
   `dmesphosted driver=board/stm32f746g-disco/wifi.ini` (and `dmspi`, and dmnet's `wifi` tool).
4. From the shell (the `wifi` tool is [dmnet's](https://github.com/choco-technologies/dmnet/tree/main/tools/wifi):
   it works with any Wi-Fi driver through dmdrvi's `DMDRVI_IOCTL_WIFI_*`):

   ```
   wifi info
   wifi scan
   wifi connect "My Network" password
   ifconfig wlan0
   ```

## Building

```bash
cmake -S . -B build -DDMOD_DIR=/path/to/dmod      # host: the tests
cmake --build build
DMOD_DMF_DIR=$PWD/build/dmf dmf-get install -d build/dmf/test_dmesphosted-local.dmd -y
DMOD_DMF_DIR=$PWD/build/dmf dmod_loader build/dmf/test_dmesphosted.dmf

cmake -S . -B build-m7 -DDMOD_TOOLS_NAME=arch/armv7/cortex-m7   # the target
cmake --build build-m7
```

The co-processor's firmware: `coprocessor/build.sh` (Docker).

## License

Apache-2.0 (as ESP-Hosted). The test fixtures are made from ESP-Hosted's
`rpc_v2.proto` (Apache-2.0, Espressif Systems).

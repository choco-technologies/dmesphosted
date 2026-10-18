# STM32F746G-DISCO + Waveshare ESP32-C3-Zero

`wifi.ini`: the Wi-Fi on the Discovery's Arduino header, the co-processor a
Waveshare ESP32-C3-Zero with [`coprocessor/`](../../../coprocessor/README.md)'s
firmware (`esp32c3-zero`).

## The wires

Both boards unplugged while wiring. The Arduino header is on the
**underside** of the Discovery (D0…D15 printed there).

| # | Signal | ESP32-C3-Zero | Discovery | STM32 |
|---|--------|---------------|-----------|-------|
| 1 | SPI CLK | GP6 | D13 | PI1 |
| 2 | SPI MOSI | GP7 | D11 | PB15 |
| 3 | SPI MISO | GP1 | D12 | PB14 |
| 4 | SPI CS | GP0 | **D5** (not D10) | PI0 |
| 5 | Handshake | GP3 | D2 | PG6 |
| 6 | Data Ready | GP4 | D4 | PG7 |
| 7 | Reset | GP5 | D7 | PI3 |
| 8 | Ground | GND | GND (between AREF and D13) | |

- **Ground only - no supply wire** between the boards: the ESP is powered by
  its own USB (which also flashes it and carries its log). Both are 3.3 V
  logic, no level shifters.
- Short wires (up to ~15 cm). The Discovery's header is female: male-female
  jumpers for a board with pins.
- D13 is also the green LED LD1 - it flickers while the bus works.
- CS is D5: D10 of this board is PA8.

## In dmod-boot

`wifi.ini` configures everything: the pins, SPI2 and the device. The modules
of the board's flash manifest:

```
dmspi
dmesphosted driver=board/stm32f746g-disco/wifi.ini
wifi
```

(dmgpio, dmhaman and dmnetif are there already; dmspi only needs to be
installed - its configuration is in `wifi.ini`.) dmnetif reports `wlan0`
like `eth0`, so `networkd.rules` and `dhcp.rules` start
`networkd@wlan0` and `dhcp@wlan0` for it: once connected it takes an
address on its own.

```
wifi info                       # the co-processor: ESP32-C3, ESP-Hosted 3.0.9, MAC
wifi scan                       # the networks around
wifi connect "My Network" password
wifi status
ifconfig wlan0
```

The network is not kept across a restart yet - connect again, or put
`ssid=` / `password=` into a copy of `wifi.ini` of your own (not this one:
it is published).

## When it does not go

The driver says what it sees (`wlan0: ...` in the log):

| Log | What |
|-----|------|
| `no answer from the co-processor - its wires, its firmware` | Nothing came in 3 s after its restart: the wires (CLK/MOSI/MISO/CS, Handshake), its firmware (`coprocessor/check.py`), its power |
| `ESP32-C3 (0x05), ESP-Hosted 3.0.9, ...` | It is reached: the bus works |
| `the co-processor speaks RPC version N` | Its firmware is another ESP-Hosted build: flash `coprocessor/`'s |
| `Wi-Fi started, MAC ...` | Its Wi-Fi runs |
| `connected to "..." (channel N)` | The link is up; DHCP follows |
| `not connected (reason N)` | esp_wifi's `wifi_err_reason_t`: 15 / 204 a wrong password, 201 no such network, 202 / 203 / 205 the access point refused |

# The co-processor: preparing an ESP board

dmesphosted's other half runs on the ESP: Espressif's
[ESP-Hosted](https://github.com/espressif/esp-hosted-mcu) co-processor
firmware, built with this directory's configuration. The ESP is then a
Wi-Fi network card on SPI - it passes 802.3 frames and takes RPC calls
(scan, connect); the TCP/IP stack stays the host's.

| What | Version |
|------|---------|
| ESP-Hosted (esp-hosted-mcu), its `examples/wifi/sta/cp` | `v3.0.9` (`ESPHOSTED_VERSION` in `build.sh`) |
| ESP-IDF | `v5.5.5` (Docker image `espressif/idf:v5.5.5`, `IDF_IMAGE`) |
| RPC | version 2 (the MCU one) - what the driver speaks |

The firmware of a board is `sdkconfig.defaults` (every board: SPI full
duplex, mode 3, checksums, the console on USB) and `boards/<board>.defaults`
(its pins). Boards so far:

| Board | Chip | `boards/` | Its pins |
|-------|------|-----------|----------|
| Waveshare ESP32-C3-Zero | ESP32-C3FN4 | `esp32c3-zero` | below |

## Preparing a board (a few minutes each)

You need:

- a USB-C cable **that carries data** (a charging-only cable powers the
  board and nothing shows up),
- Python 3 and esptool: `pip install esptool` (or `pipx install esptool`),
- the firmware image: `dmesphosted-coprocessor-<board>-v<version>.bin` from
  the [releases](https://github.com/choco-technologies/dmesphosted/releases)
  (or one you built, see below),
- this directory (`flash.sh`, `check.py`, `boards/`).

1. **Connect the board.** It is seen by its own USB, no programmer needed:

   ```
   $ lsusb | grep 303a
   Bus 001 Device 011: ID 303a:1001 Espressif USB JTAG/serial debug unit
   $ ls /dev/serial/by-id/ | grep Espressif
   usb-Espressif_USB_JTAG_serial_debug_unit_9C:CC:01:A8:1E:68-if00
   ```

   Nothing there: another cable (see above), another USB port. On Linux your
   user has to be allowed to open the port (group `dialout` or `plugdev`,
   depending on the distribution).

2. **Write it and check it** - one command:

   ```
   ./flash.sh --backup factory-<MAC>.bin dmesphosted-coprocessor-esp32c3-zero-v0.1.bin
   ```

   `--backup` first saves what the board came with (a few seconds; keep it
   if you may want the board back as it was). Then esptool writes the image
   and `check.py` restarts the board and reads its start:

   ```
     ok   ESP-Hosted EH-3.0.9
     ok   transport: SPI
     ok   CLK = GPIO6 (it says 6)
     ...
     ok   ready for the host
   OK
   ```

   Every line `ok` and `OK` at the end: the board is ready. A `FAIL` line
   says what differs, and the board's log follows.

3. **Write the board's MAC on it** (esptool prints it: `MAC: 9c:cc:01:...`) -
   it is the Wi-Fi's MAC, the one a router shows.

### When it does not go

| What you see | What to do |
|--------------|------------|
| `no ESP on USB` | Step 1: the cable, the port |
| esptool: `Failed to connect` | The download mode by hand: hold **BOOT**, press and release **RESET**, release BOOT, run `flash.sh` again. Then press RESET once more after it is written |
| `check.py`: nothing in the log | Press RESET; run `python3 check.py -b boards/<board>.defaults -p /dev/ttyACM<n> -v` |
| `FAIL` on a pin | The image is not this board's - another `boards/` was built into it |

### The board back as it was

```
esptool --chip esp32c3 write_flash 0x0 factory-<MAC>.bin
```

## Building the firmware

Only to change something (a pin, a version, another board) - otherwise take
the release's. Docker is all it needs:

```
./build.sh [board]        # default esp32c3-zero
```

The first run clones esp-hosted-mcu `v3.0.9` into `build/esp_hosted` and
pulls the ESP-IDF image (a few GB, once). Out:
`build/<board>/dmesphosted-coprocessor-<board>.bin`, which `flash.sh` takes
when it is given no image. The sdkconfig is made from the defaults on every
build - change the `.defaults`, never `build/<board>/sdkconfig`.

Another ESP-Hosted version: delete `build/esp_hosted`, run with
`ESPHOSTED_VERSION=v3.x.y ./build.sh`, and change the default in `build.sh`
once it works. The driver checks the RPC version the co-processor reports;
the protocol must stay RPC version 2.

### Another board

1. `boards/<board>.defaults` - the pins (the `CONFIG_EH_TRANSPORT_CP_SPI_*`
   of `boards/esp32c3-zero.defaults`). Keep off the chip's strapping pins,
   its USB and whatever the board has on its pins (LEDs, buttons) - look at
   the board's pinout and the chip's datasheet.
2. `boards/<board>.target` - one line, the chip (`esp32c6`, ...), when it is
   not an ESP32-C3.
3. A row in the table above, and the board in `.github/workflows/release.yml`.

## Waveshare ESP32-C3-Zero

| Signal | ESP32-C3-Zero | Direction |
|--------|---------------|-----------|
| SPI CLK | GP6 | host → ESP |
| SPI MOSI | GP7 | host → ESP |
| SPI MISO | GP1 | ESP → host |
| SPI CS | GP0 | host → ESP |
| Handshake | GP3 | ESP → host |
| Data Ready | GP4 | ESP → host |
| Reset | GP5 | host → ESP (a LOW pulse ≥ 2 ms restarts it) |
| Ground | GND | |

Not used: GP2, GP8, GP9 (strapping; GP9 is BOOT), GP10 (the RGB LED),
GP18/GP19 (USB), GP20/GP21 (UART0). The host's side of the wires: the
board's configuration, e.g.
[`configs/board/stm32f746g-disco`](../configs/board/stm32f746g-disco/README.md).

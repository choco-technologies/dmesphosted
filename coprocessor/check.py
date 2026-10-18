#!/usr/bin/env python3
"""
Restarts an ESP board running the co-processor's firmware and checks, by its
start log (the chip's USB serial/JTAG), that it is what boards/<board>.defaults
says: ESP-Hosted, over SPI, on the board's pins.

    check.py -b boards/esp32c3-zero.defaults -p /dev/ttyACM1 [-t seconds] [-v]
    check.py -b boards/esp32c3-zero.defaults --log saved.log

Exit code 0: it is; 1: it is not (the log is printed); 2: no such port.
Only Python's own library - no pyserial needed.
"""
import argparse
import fcntl
import os
import re
import select
import struct
import sys
import termios
import time

PINS = {
    "CLK": "CONFIG_EH_TRANSPORT_CP_SPI_HSPI_GPIO_CLK",
    "MOSI": "CONFIG_EH_TRANSPORT_CP_SPI_HSPI_GPIO_MOSI",
    "MISO": "CONFIG_EH_TRANSPORT_CP_SPI_HSPI_GPIO_MISO",
    "CS": "CONFIG_EH_TRANSPORT_CP_SPI_HSPI_GPIO_CS",
    "HANDSHAKE": "CONFIG_EH_TRANSPORT_CP_SPI_GPIO_HANDSHAKE",
    "DATA_READY": "CONFIG_EH_TRANSPORT_CP_SPI_GPIO_DATA_READY",
    "RESET": "CONFIG_EH_TRANSPORT_CP_SPI_GPIO_RESET",
}


def board_pins(path):
    values = dict(re.findall(r"^(CONFIG_\w+)=(-?\d+)", open(path).read(), re.M))
    return {name: int(values[key]) for name, key in PINS.items() if key in values}


def set_line(fd, bit, on):
    fcntl.ioctl(fd, termios.TIOCMBIS if on else termios.TIOCMBIC, struct.pack("I", bit))


def open_port(port):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = attrs[1] = attrs[3] = 0          # raw: no input, output, local processing
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[4] = attrs[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def read_start(port, seconds):
    """The chip's log from a restart (RTS, as esptool resets it) on"""
    fd = open_port(port)
    set_line(fd, termios.TIOCM_DTR, False)
    set_line(fd, termios.TIOCM_RTS, True)
    time.sleep(0.1)
    set_line(fd, termios.TIOCM_RTS, False)

    out, end = b"", time.time() + seconds
    while time.time() < end:
        try:
            if select.select([fd], [], [], 0.2)[0]:
                out += os.read(fd, 4096)
        except OSError:                         # the USB went away for a moment
            os.close(fd)
            time.sleep(0.3)
            try:
                fd = open_port(port)
            except OSError:
                pass
    os.close(fd)
    return re.sub(r"\x1b\[[0-9;]*m", "", out.decode(errors="replace"))


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("-b", "--board", required=True, help="boards/<board>.defaults")
    ap.add_argument("-p", "--port")
    ap.add_argument("--log", help="check a saved log instead of a board")
    ap.add_argument("-t", "--time", type=float, default=6.0, help="seconds to listen")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the whole log")
    args = ap.parse_args()

    pins = board_pins(args.board)
    if args.log:
        log = re.sub(r"\x1b\[[0-9;]*m", "", open(args.log, errors="replace").read())
    elif args.port and os.path.exists(args.port):
        log = read_start(args.port, args.time)
    else:
        print(f"no {args.port}" if args.port else "give -p port or --log file", file=sys.stderr)
        return 2
    if args.verbose:
        print(log)

    checks = []
    version = re.search(r"ESP-Hosted Firmware version :: (\S+)", log)
    checks.append(("ESP-Hosted " + (version.group(1) if version else "?"), version is not None))
    checks.append(("transport: SPI", "Transport used :: SPI" in log))
    bus = re.search(r"transport\[cp\]: SPI ctrl=\d+ mode=(\d+) (.*)", log)
    seen = dict(re.findall(r"(\w+)=(\d+)", bus.group(2))) if bus else {}
    for name in ("CLK", "MOSI", "MISO", "CS", "HANDSHAKE", "DATA_READY"):
        if name in pins:
            got = seen.get(name)
            checks.append((f"{name} = GPIO{pins[name]} (it says {got})", got == str(pins[name])))
    if bus:
        checks.append((f"SPI mode 3 (it says {bus.group(1)})", bus.group(1) == "3"))
    if pins.get("RESET", -1) >= 0:
        checks.append((f"reset = GPIO{pins['RESET']}",
                       f"Using GPIO [{pins['RESET']}] as slave reset pin" in log))
    checks.append(("ready for the host", "Send slave up event" in log))

    for text, ok in checks:
        print(("  ok   " if ok else "  FAIL ") + text)
    if all(ok for _, ok in checks):
        print("OK")
        return 0
    if not args.verbose:
        print("--- its log ---\n" + (log or "(nothing - is it running? press RESET and try again)"))
    return 1


if __name__ == "__main__":
    sys.exit(main())

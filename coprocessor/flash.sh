#!/bin/sh
#
# Writes the co-processor's firmware onto an ESP board over the board's own
# USB, then checks that it starts as the board's firmware should (check.py).
#
#   ./flash.sh [-b board] [-p port] [--backup file] [image.bin]
#
#   -b board        boards/<board>.defaults: what to check (default esp32c3-zero)
#   -p port         the board's serial port (default: the one ESP on USB)
#   --backup file   first reads what is on the flash into file
#   image.bin       the image (default: build/<board>/..., what build.sh made;
#                   or the one of a release)
#
# Needs esptool (pip install esptool) and python3.
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
board=esp32c3-zero
port=""
backup=""
image=""

die() { echo "$0: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        -b) board=$2; shift 2 ;;
        -p) port=$2; shift 2 ;;
        --backup) backup=$2; shift 2 ;;
        -h|--help) sed -n '3,15p' "$0"; exit 0 ;;
        -*) die "unknown option $1" ;;
        *) image=$1; shift ;;
    esac
done

[ -f "$here/boards/$board.defaults" ] || die "no boards/$board.defaults"
[ -n "$image" ] || image=$here/build/$board/dmesphosted-coprocessor-$board.bin
[ -f "$image" ] || die "no $image - build it (./build.sh $board) or give the release's"

chip=esp32c3
[ -f "$here/boards/$board.target" ] && chip=$(cat "$here/boards/$board.target")

if [ -z "$port" ]; then
    set -- /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*
    [ -e "$1" ] || die "no ESP on USB - a cable that carries data? (lsusb: 303a:1001 Espressif)"
    [ $# -eq 1 ] || die "more than one ESP on USB - give -p"
    port=$(readlink -f "$1")
fi

esptool=$(command -v esptool || command -v esptool.py || true)
[ -n "$esptool" ] || die "no esptool - pip install esptool"

if [ -n "$backup" ]; then
    "$esptool" --chip "$chip" -p "$port" read_flash 0 ALL "$backup"
fi

# All of it erased first: nothing of an earlier firmware stays (its NVS -
# Wi-Fi settings - outside the image). --after no_reset: check.py restarts
# it, so that it sees the whole start.
"$esptool" --chip "$chip" -p "$port" erase_flash
"$esptool" --chip "$chip" -p "$port" --after no_reset write_flash 0x0 "$image"

python3 "$here/check.py" -b "$here/boards/$board.defaults" -p "$port"

#!/bin/sh
#
# Builds the co-processor's firmware for a board: ESP-Hosted's co-processor
# (esp-hosted-mcu ESPHOSTED_VERSION, examples/wifi/sta/cp) with
# sdkconfig.defaults and boards/<board>.defaults of this directory, in
# ESP-IDF's Docker image - nothing but Docker is needed (in ESP-IDF's own
# environment - idf.py there - it runs idf.py itself).
#
#   ./build.sh [board]          (default: esp32c3-zero)
#
# Out: build/<board>/dmesphosted-coprocessor-<board>.bin - the whole flash
# image (bootloader, partition table, application), written at 0x0 by
# flash.sh.
#
# The source is cloned into build/esp_hosted once (under the component's
# name: the example finds it by it); delete it to take another version.
#
set -eu

here=$(cd "$(dirname "$0")" && pwd)
IDF_IMAGE=${IDF_IMAGE:-espressif/idf:v5.5.5}
ESPHOSTED_VERSION=${ESPHOSTED_VERSION:-v3.0.9}
board=${1:-esp32c3-zero}

defaults=$here/boards/$board.defaults
[ -f "$defaults" ] || { echo "$0: no boards/$board.defaults" >&2; exit 1; }

# The chip of a board: boards/<board>.target, else the ESP32-C3
target=esp32c3
[ -f "$here/boards/$board.target" ] && target=$(cat "$here/boards/$board.target")

src=$here/build/esp_hosted
if [ ! -d "$src" ]; then
    git clone --depth 1 --branch "$ESPHOSTED_VERSION" --recurse-submodules \
        --shallow-submodules https://github.com/espressif/esp-hosted-mcu.git "$src"
fi

# The sdkconfig is made from the defaults every time: they are what the
# firmware is, not an sdkconfig left over from an earlier build
out=$here/build/$board
mkdir -p "$out"
rm -f "$out/sdkconfig"

defaults="sdkconfig.defaults;__HERE__/sdkconfig.defaults;__HERE__/boards/$board.defaults"

if command -v idf.py >/dev/null 2>&1 && [ -n "${IDF_PATH:-}" ]; then
    # In ESP-IDF's environment already (its image in CI): as it is
    cd "$src/examples/wifi/sta/cp"
    idf.py -B "$out" -DIDF_TARGET="$target" -DSDKCONFIG="$out/sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$(echo "$defaults" | sed "s|__HERE__|$here|g")" \
        build merge-bin -o "dmesphosted-coprocessor-$board.bin"
else
    tty=""
    [ -t 0 ] && tty="-it"

    # shellcheck disable=SC2086 # $tty is a list of words
    docker run --rm $tty -u "$(id -u):$(id -g)" -e HOME=/tmp \
        -v "$here:/work" -w /work/build/esp_hosted/examples/wifi/sta/cp \
        "$IDF_IMAGE" \
        idf.py -B "/work/build/$board" -DIDF_TARGET="$target" \
            -DSDKCONFIG="/work/build/$board/sdkconfig" \
            -DSDKCONFIG_DEFAULTS="$(echo "$defaults" | sed "s|__HERE__|/work|g")" \
            build merge-bin -o "dmesphosted-coprocessor-$board.bin"
fi

echo "$out/dmesphosted-coprocessor-$board.bin"

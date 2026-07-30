#!/bin/bash
# Flash the XIAO nRF52840 via serial DFU over the bootloader's CDC-ACM,
# for when the Adafruit UF2 bootloader's mass-storage endpoint isn't
# enumerating and `tools/flash.sh` fails with "XIAO-SENSE never appeared".
# See memory: reference_serial_dfu_flash_fallback.
#
# What it does:
#   1. SSHes to the Pi (`rpi-xiao` alias) and runs ~/xiao-bootloader.sh
#      to double-tap-reset the XIAO into its bootloader (same as flash.sh).
#   2. Waits for the bootloader's /dev/cu.usbmodem* to appear on the Mac.
#   3. Packages the .hex into a DFU zip with adafruit-nrfutil.
#   4. Flashes it over CDC-ACM with `adafruit-nrfutil dfu serial`.
#
# Requires:
#   - `adafruit-nrfutil` on PATH (`pip3 install --user adafruit-nrfutil`;
#     the user-scope bin typically ends up at ~/Library/Python/3.9/bin).
#   - Same Pi/SSH setup as tools/flash.sh (rpi-xiao alias + xiao-bootloader.sh).
#
# Usage:
#   ./tools/flash-serial.sh                       # flashes the standard build's .hex
#   ./tools/flash-serial.sh path/to/other.hex     # flashes a specific .hex

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HEX="${1:-$REPO_ROOT/seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.hex}"
PI_ALIAS="rpi-xiao"
DFU_ZIP="/tmp/xiao-app-dfu.zip"

# Adafruit-nrfutil installs to a user-scope bin that isn't on the default
# interactive PATH. Add the common macOS location if it's not already there.
for cand in "$HOME/Library/Python/3.9/bin" "$HOME/Library/Python/3.11/bin" "$HOME/Library/Python/3.12/bin"; do
    if [ -d "$cand" ] && [[ ":$PATH:" != *":$cand:"* ]]; then
        PATH="$cand:$PATH"
    fi
done

if ! command -v adafruit-nrfutil >/dev/null 2>&1; then
    echo "error: adafruit-nrfutil not found on PATH" >&2
    echo "  install with: pip3 install --user adafruit-nrfutil" >&2
    exit 1
fi

if [ ! -f "$HEX" ]; then
    echo "error: hex not found at $HEX" >&2
    echo "usage: $0 [path/to/zephyr.hex]" >&2
    exit 1
fi

echo "flashing $HEX ($(du -h "$HEX" | cut -f1))"

echo "-> triggering bootloader via Pi..."
# shellcheck disable=SC2088  # tilde is expanded by the Pi's login shell, not locally
if ! ssh "$PI_ALIAS" '~/xiao-bootloader.sh' > /dev/null; then
    echo "error: could not reach $PI_ALIAS or ~/xiao-bootloader.sh failed" >&2
    echo "hint: try 'ssh $PI_ALIAS uptime' to check connectivity" >&2
    exit 1
fi

# CDC-ACM enumeration after double-tap takes a beat. Poll for it — /dev/tty.*
# blocks on DCD which CDC-ACM doesn't assert, so use cu.* (see memory:
# project_macos_cu_vs_tty_usbmodem).
echo "-> waiting for bootloader CDC-ACM port..."
PORT=""
for i in $(seq 1 15); do
    # shellcheck disable=SC2012
    PORT="$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)"
    if [ -n "$PORT" ]; then
        echo "   found $PORT after ${i}s"
        break
    fi
    sleep 1
done
if [ -z "$PORT" ]; then
    echo "error: no /dev/cu.usbmodem* appeared" >&2
    echo "  bootloader CDC-ACM never enumerated. If MSC is also gone," >&2
    echo "  the bootloader itself may need re-flashing over SWD." >&2
    exit 1
fi

echo "-> packaging $HEX -> $DFU_ZIP..."
if ! adafruit-nrfutil dfu genpkg \
        --dev-type 0x0052 \
        --application "$HEX" \
        --sd-req 0xFFFE \
        "$DFU_ZIP" > /dev/null; then
    echo "error: adafruit-nrfutil dfu genpkg failed" >&2
    exit 1
fi

echo "-> flashing over $PORT..."
if ! adafruit-nrfutil dfu serial \
        --package "$DFU_ZIP" \
        --port "$PORT" \
        -b 115200 --singlebank; then
    echo "error: serial DFU failed" >&2
    exit 1
fi

echo "done. XIAO should boot into the new firmware within a few seconds."

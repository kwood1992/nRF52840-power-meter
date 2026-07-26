#!/bin/bash
# Flash the XIAO nRF52840 via SWD from the Pi — bypasses the bootloader's
# USB endpoints entirely. Use when USB is disconnected (e.g. current-
# measurement runs with the board powered from an external supply).
#
# Companion scripts:
#   tools/flash.sh         — normal USB drag-drop path (fastest when USB is up)
#   tools/flash-serial.sh  — CDC-ACM DFU fallback (USB up, MSC broken)
#   tools/flash-swd.sh     — this script (no USB at all)
#
# WARNING — power hazard:
#   Do NOT leave USB plugged into the XIAO while the board is also powered
#   from the Pi 3V3 → BAT/VDD rail. Two supplies in parallel through
#   different regulators is not a safe steady state. Either unplug USB
#   before flashing over SWD, or bench-test the pairing very briefly
#   before walking away.
#
# What it does:
#   1. Refuses if the .hex covers reserved flash regions (< 0x27000). This
#      guards against accidentally flashing merged.hex, which includes the
#      MBR + SoftDevice and would trash the Adafruit UF2 bootloader.
#   2. scp's the .hex to the Pi's /tmp.
#   3. Runs OpenOCD on the Pi against the SWD wires (same interface config
#      as the bootloader-recovery command), using `program … verify` to
#      write and read-back the app slot only.
#   4. Greps the OpenOCD output for "** Verified OK **"; non-zero exit if
#      absent.
#
# Requires:
#   - Same rpi-xiao SSH alias + passwordless-sudo openocd setup used by
#     the bootloader-recovery flow.
#   - The Pi's SWD wires physically connected to the XIAO's SWD pads.
#
# Usage:
#   ./tools/flash-swd.sh                          # flashes the standard build's .hex
#   ./tools/flash-swd.sh path/to/other.hex        # flashes a specific .hex
#
# Do NOT pass merged.hex here — the address-safety check will (correctly)
# refuse it. Re-flashing the SoftDevice on every tuning cycle is neither
# needed nor safe.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HEX="${1:-$REPO_ROOT/seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.hex}"
PI_ALIAS="rpi-xiao"
PI_TMP_HEX="/tmp/zephyr.hex"
MIN_APP_ADDR=$((0x27000))

if [ ! -f "$HEX" ]; then
    echo "error: hex not found at $HEX" >&2
    echo "usage: $0 [path/to/zephyr.hex]" >&2
    exit 1
fi

# ---- address-safety check --------------------------------------------------
# Parse the first extended-address record (Intel-HEX type 02 = ESA, type 04
# = ELA) to establish the base address, then combine with the first data
# record's offset. Refuse if the resulting start address is below 0x27000,
# which is where the Nordic MBR + S140 SoftDevice sit (see
# project_xiao_flash_reserved_regions). Pure-bash — macOS's stock awk
# doesn't have gawk's strtonum().
START_ADDR_INT=""
BASE=0
while IFS= read -r line; do
    # Record format: :LLAAAATT[DD..]CC — hex nibbles after the leading colon.
    [ "${line:0:1}" = ":" ] || continue
    rec_type="${line:7:2}"
    case "$rec_type" in
        02) BASE=$(( 0x${line:9:4} << 4 )) ;;   # Extended Segment Address
        04) BASE=$(( 0x${line:9:4} << 16 )) ;;  # Extended Linear Address
        00)
            offset=$(( 0x${line:3:4} ))
            START_ADDR_INT=$(( BASE + offset ))
            break
            ;;
    esac
done < "$HEX"

if [ -z "$START_ADDR_INT" ]; then
    echo "error: could not parse a data record from $HEX" >&2
    exit 1
fi
printf "hex start address: 0x%x\n" "$START_ADDR_INT"

if [ "$START_ADDR_INT" -lt "$MIN_APP_ADDR" ]; then
    printf 'error: hex starts at 0x%x, below the app-slot floor 0x%x.\n' \
        "$START_ADDR_INT" "$MIN_APP_ADDR" >&2
    echo "  This is almost certainly merged.hex (includes MBR + SoftDevice)." >&2
    echo "  Flashing it over SWD would clobber the SoftDevice on every cycle." >&2
    echo "  Pass zephyr.hex (app-only) instead." >&2
    exit 1
fi
# ---- end address-safety check ---------------------------------------------

echo "flashing $HEX ($(du -h "$HEX" | cut -f1)) via SWD on $PI_ALIAS"

echo "-> copying hex to $PI_ALIAS:$PI_TMP_HEX..."
if ! scp -q "$HEX" "$PI_ALIAS:$PI_TMP_HEX"; then
    echo "error: scp to $PI_ALIAS failed" >&2
    exit 1
fi

# Match the bootloader-recovery invocation's structure (see
# reference_openocd_swd_recovery memory), minus `nrf5 mass_erase` — we want
# to preserve MBR + SoftDevice + bootloader across app flashes.
OPENOCD_LOG="$(mktemp -t flash-swd-openocd.XXXXXX)"
trap 'rm -f "$OPENOCD_LOG"' EXIT

echo "-> running OpenOCD program+verify..."
if ! ssh "$PI_ALIAS" "sudo openocd \
    -f interface/raspberrypi-swd.cfg \
    -c 'transport select swd' \
    -c 'adapter speed 1000' \
    -f target/nordic/nrf52.cfg \
    -c 'init; reset halt; program $PI_TMP_HEX verify; reset run; exit'" \
    > "$OPENOCD_LOG" 2>&1; then
    echo "error: openocd exited non-zero" >&2
    sed 's/^/  /' "$OPENOCD_LOG" >&2
    exit 1
fi

if ! grep -q '\*\* Verified OK \*\*' "$OPENOCD_LOG"; then
    echo "error: OpenOCD did not report '** Verified OK **' — flash may be incomplete" >&2
    sed 's/^/  /' "$OPENOCD_LOG" >&2
    exit 1
fi

echo "done. XIAO reset into new firmware."

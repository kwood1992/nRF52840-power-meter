#!/bin/bash
# Flash the XIAO nRF52840 Sense via SWD-triggered bootloader entry
# — no reset button press needed.
#
# What it does:
#   1. SSHes to the Pi (`rpi-xiao` alias) and runs ~/xiao-bootloader.sh,
#      which pulses GPIO 23 twice to emulate the Adafruit UF2 bootloader's
#      double-tap-reset detection.
#   2. Waits for the XIAO-SENSE volume to mount on the Mac.
#   3. Copies the UF2 to it (which auto-flashes and reboots).
#   4. Waits for the volume to unmount as confirmation the flash completed.
#
# Requires:
#   - A Raspberry Pi wired to the XIAO's SWD pads (see
#     CONTRIBUTING.md / working notes for pin mapping) with GPIO 23 → RST
#   - `~/xiao-bootloader.sh` installed on the Pi
#   - `rpi-xiao` SSH alias in ~/.ssh/config with key-based auth
#
# Usage:
#   ./tools/flash.sh                       # flashes the standard build
#   ./tools/flash.sh path/to/other.uf2     # flashes a specific UF2

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UF2="${1:-$REPO_ROOT/seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.uf2}"
DRIVE="/Volumes/XIAO-SENSE"
PI_ALIAS="rpi-xiao"

if [ ! -f "$UF2" ]; then
    echo "error: UF2 not found at $UF2" >&2
    echo "usage: $0 [path/to/zephyr.uf2]" >&2
    exit 1
fi

echo "flashing $UF2 ($(du -h "$UF2" | cut -f1))"

echo "-> triggering bootloader via Pi..."
if ! ssh "$PI_ALIAS" '~/xiao-bootloader.sh' > /dev/null; then
    echo "error: could not reach $PI_ALIAS or ~/xiao-bootloader.sh failed" >&2
    echo "hint: try 'ssh $PI_ALIAS uptime' to check connectivity" >&2
    exit 1
fi

echo "-> waiting for $DRIVE to mount..."
for i in $(seq 1 15); do
    if [ -d "$DRIVE" ]; then
        echo "   mounted after ${i}s"
        break
    fi
    sleep 1
done
if [ ! -d "$DRIVE" ]; then
    echo "error: $DRIVE never appeared" >&2
    echo "  is the XIAO plugged in via USB? Is SWD RESET wired? (Pi GPIO 23 -> RST pad)" >&2
    exit 1
fi

# Settle delay: the drive being visible in `ls` doesn't mean the FAT
# filesystem is ready for writes. cp too soon after mount races with
# macOS finishing the filesystem probe and errors out completely
# (payload never lands), leaving the drive stuck mounted.
sleep 2

echo "-> copying UF2..."
# Plain `cp` is what the Adafruit UF2 bootloader expects on macOS. The
# payload lands via `fcopyfile`; then the bootloader auto-unmounts the
# drive as it commits to flash, so cp's followup metadata steps
# (fchmod, xattr copy) always fail with "Device not configured".
# `2>/dev/null` swallows that noise; `|| true` keeps the script alive
# so the drive-unmount check below decides success. (COPYFILE_DISABLE=1
# and other `cp` variants tested — `-X`, `dd`, `ditto` — either
# silently skip the payload or leave metadata companion files that
# the bootloader chokes on.)
cp "$UF2" "$DRIVE/" 2>/dev/null || true

echo "-> waiting for drive to unmount (indicates flash accepted)..."
for i in $(seq 1 20); do
    if [ ! -d "$DRIVE" ]; then
        echo "   unmounted after ${i}s — flash accepted, XIAO rebooting"
        exit 0
    fi
    sleep 1
done

echo "warn: drive still mounted after 20 s — UF2 may have been rejected" >&2
echo "  common causes: corrupt UF2, wrong family ID, target address outside app slot" >&2
exit 1

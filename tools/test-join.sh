#!/bin/bash
# End-to-end Zigbee join/interview regression test.
#
# Sequence:
#   1. Flash the given UF2 (defaults to the standard build output)
#   2. Wait for boot
#   3. Enable permit_join in Z2M FIRST — needs to be on before the
#      device starts scanning, otherwise the coordinator rejects the
#      association and no device_joined event fires.
#   4. Factory-reset via long-press. The reset flow is:
#        * ZBOSS emits LEAVE, wipes zboss_nvram, calls sys_reset
#        * CPU reboots, ZBOSS re-inits with empty NVRAM
#        * FIRST_START signal fires
#        * light_switch-pattern default handler auto-steers
#      That whole cycle takes ~15-25 s. No button-press to trigger
#      steering needed — the default handler already does it.
#   5. Watch zigbee2mqtt/bridge/event for our device's IEEE up to 90 s
#      → success on device_interview_successful
#      → failure on device_interview_failed OR timeout
#   6. Disable permit_join, print outcome, exit
#
# Requires (all documented in docs/swd-recovery-jig.md):
#   - Pi SWD flash rig (tools/flash.sh) OR serial-DFU fallback (tools/flash-serial.sh)
#   - Pi wired to XIAO button (GPIO 17 -> D6)
#   - ~/z2m-cli on the Pi with valid MQTT creds in ~/.mosquitto-xiao-creds
#
# Usage:
#   ./tools/test-join.sh                  # test the standard build
#   ./tools/test-join.sh path/to/other.uf2
#
# Flash strategy: tries tools/flash.sh (MSC drop) first, since it's the
# fastest and doesn't need adafruit-nrfutil. Falls back to
# tools/flash-serial.sh if the mass-storage endpoint doesn't advertise
# (a recurring failure mode — see memory
# reference_serial_dfu_flash_fallback). flash-serial packages the .hex,
# not the .uf2, so we hand it the sibling .hex for the same build.

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UF2="${1:-$REPO_ROOT/seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.uf2}"
PI_ALIAS="rpi-xiao"
INTERVIEW_TIMEOUT_S=90

# Read the XIAO's IEEE from the Pi's creds file so we filter events for
# just our device. If you swap boards, update the creds file.
XIAO_IEEE="$(ssh "$PI_ALIAS" 'grep XIAO_FRIENDLY_NAME ~/.mosquitto-xiao-creds | cut -d= -f2')"
if [ -z "$XIAO_IEEE" ]; then
    echo "error: could not read XIAO_FRIENDLY_NAME from Pi's creds file" >&2
    exit 1
fi

log() {
    echo "[test-join] $*"
}

log "target device: $XIAO_IEEE"
log "UF2:           $UF2"

# -- 1. flash --
# Try flash.sh (MSC drop) first. If it fails (typically because
# /Volumes/XIAO-SENSE doesn't mount), fall back to flash-serial.sh
# with the sibling .hex.
log "1/5 flashing (trying flash.sh first)..."
if "$REPO_ROOT/tools/flash.sh" "$UF2" >/dev/null 2>&1; then
    log "     flash.sh succeeded"
else
    HEX="${UF2%.uf2}.hex"
    if [ ! -f "$HEX" ]; then
        log "error: flash.sh failed AND no sibling .hex found for flash-serial fallback (looked for $HEX)"
        exit 3
    fi
    log "     flash.sh failed — falling back to flash-serial.sh with $(basename "$HEX")..."
    if ! "$REPO_ROOT/tools/flash-serial.sh" "$HEX" >/dev/null 2>&1; then
        log "error: both flash paths failed"
        exit 3
    fi
    log "     flash-serial.sh succeeded"
fi

# -- 2. wait for boot --
log "2/5 waiting 8 s for boot..."
sleep 8

# -- 3. permit_join ON *before* the factory reset --
log "3/5 permit_join true (must precede the reset — coordinator needs to accept the auto-steer)..."
ssh "$PI_ALIAS" '~/z2m-cli permit-join true' >/dev/null

# -- 4. factory reset (device reboots → FIRST_START → default handler auto-steers) --
log "4/5 factory reset (long-press ~4 s)..."
ssh "$PI_ALIAS" '~/xiao-long-press.sh'

# -- 5. watch for the interview to complete --
log "5/5 watching zigbee2mqtt/bridge/event (timeout ${INTERVIEW_TIMEOUT_S}s)..."
# Poll bridge/devices for our device's interview_state. Z2M's
# bridge/event API in 2.x doesn't emit a specific
# "device_interview_successful" event, but the interview_state field
# on the device object flips to SUCCESSFUL / FAILED when interview
# terminates, so we poll for that.
OUTCOME=""
JOINED_LOGGED=0
for i in $(seq 1 $((INTERVIEW_TIMEOUT_S / 3))); do
    STATE=$(ssh "$PI_ALIAS" 'timeout 3 ~/z2m-cli sub bridge/devices -C 1 2>/dev/null' \
        | python3 -c "
import json, sys
target = '$XIAO_IEEE'
try:
    data = json.load(sys.stdin)
except Exception:
    print('NO_DATA'); sys.exit(0)
for d in data:
    if d.get('ieee_address', '').lower() == target:
        state = d.get('interview_state') or 'UNKNOWN'
        completed = d.get('interview_completed', False)
        print(f'{state}|{completed}')
        break
else:
    print('NOT_JOINED')
" 2>/dev/null || echo "QUERY_ERROR")
    STATE_NAME=$(echo "$STATE" | cut -d'|' -f1)

    if [ "$STATE_NAME" = SUCCESSFUL ]; then
        OUTCOME=SUCCESS
        break
    elif [ "$STATE_NAME" = FAILED ]; then
        OUTCOME=FAILED
        break
    elif [ "$STATE_NAME" != NOT_JOINED ] && [ $JOINED_LOGGED -eq 0 ]; then
        log "   device in Z2M (state=$STATE_NAME) — waiting for interview to complete"
        JOINED_LOGGED=1
    fi
    sleep 3
done

# Disable permit_join no matter the outcome
ssh "$PI_ALIAS" '~/z2m-cli permit-join false' >/dev/null 2>&1 || true

if [ -z "$OUTCOME" ]; then
    OUTCOME=TIMEOUT
fi

log "final interview_state: $STATE"

case "$OUTCOME" in
    SUCCESS)
        log "PASS — join + interview completed"
        exit 0
        ;;
    FAILED)
        log "FAIL — Z2M reported interview failure"
        exit 1
        ;;
    TIMEOUT)
        log "FAIL — no terminal event in ${INTERVIEW_TIMEOUT_S}s"
        exit 2
        ;;
esac

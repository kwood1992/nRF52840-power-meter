#!/bin/bash
# End-to-end Zigbee join/interview regression test.
#
# Sequence:
#   1. Flash the given UF2 (defaults to the standard build output)
#   2. Wait for boot
#   3. Remove Z2M's cached device entry AND verify it's gone. Z2M keys
#      interview_state, configured_reportings and the endpoint CLUSTER LIST
#      by IEEE, and a device-side factory reset invalidates none of it — so
#      without this the test can pass off the previous join's cache (#20,
#      #69) and can report a stale cluster list as current (#62). Removing
#      first makes the start state provable and forces a simple-descriptor
#      re-read. The verify matters as much as the remove: an unverified
#      remove leaves the whole pass/fail signal resting on an assumption.
#   4. Enable permit_join in Z2M — needs to be on before the
#      device starts scanning, otherwise the coordinator rejects the
#      association and no device_joined event fires.
#   5. Factory-reset via long-press. The reset flow is:
#        * ZBOSS emits LEAVE, wipes zboss_nvram, calls sys_reset
#        * CPU reboots, ZBOSS re-inits with empty NVRAM
#        * FIRST_START signal fires
#        * light_switch-pattern default handler auto-steers
#      That whole cycle takes ~15-25 s. No button-press to trigger
#      steering needed — the default handler already does it.
#   6. Poll for interview completion up to 90 s, report the advertised
#      cluster list, optionally assert it, disable permit_join, exit.
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
# Env EXPECT_CLUSTERS asserts the device's advertised input clusters, so a
# firmware cluster-list change can be regression-tested deliberately. Two
# forms are accepted:
#
#   flat            EXPECT_CLUSTERS=genBasic,genIdentify,genPollCtrl,seMetering
#                   Compares the union of input clusters, and additionally
#                   requires that exactly ONE endpoint advertises them. The
#                   firmware declares a single endpoint (APP_ENDPOINT 10,
#                   ZBOSS_DECLARE_DEVICE_CTX_1_EP), so that holds today.
#
#   per-endpoint    EXPECT_CLUSTERS='10:genBasic,genIdentify,genPollCtrl,seMetering'
#                   Pins each cluster to its endpoint. Use this when the
#                   firmware grows a second endpoint — the flat form cannot
#                   tell "moved to another endpoint" from "still here", and
#                   a cluster moving endpoints breaks Z2M/HA bindings even
#                   though the union is unchanged.
#
# Order doesn't matter (both sides are sorted); exits 4 on mismatch.
#
# Flash strategy: chained fallback in the order flash.sh (USB MSC drop) →
# flash-serial.sh (USB CDC-ACM DFU) → flash-swd.sh (SWD via Pi OpenOCD).
# USB paths run first because they're the fastest and don't touch the
# SWD wiring; SWD is the final fallback because it works even when USB
# is unplugged entirely — the INA219-measurement scenario (board on
# Pi 3V3 rail, USB removed to avoid the two-supply hazard).
#
# All three flash paths need the sibling .hex for the given .uf2 (SWD
# and CDC-DFU package the .hex; only flash.sh uses the .uf2 directly).
#
# Env FLASH_METHOD short-circuits the chain when the caller knows which
# path will succeed and wants to skip the USB timeouts:
#   FLASH_METHOD=swd  → SWD only (fastest when USB is knowingly unplugged)
#   FLASH_METHOD=usb  → flash.sh + flash-serial.sh only, no SWD
#   unset (default)   → auto: try all three, in the order above

set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
UF2="${1:-$REPO_ROOT/seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.uf2}"
PI_ALIAS="rpi-xiao"
INTERVIEW_TIMEOUT_S=90
FLASH_METHOD="${FLASH_METHOD:-auto}"

case "$FLASH_METHOD" in
    auto|usb|swd) ;;
    *)
        echo "error: FLASH_METHOD must be one of: auto (default), usb, swd (got: $FLASH_METHOD)" >&2
        exit 2
        ;;
esac

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
# Chained fallback governed by FLASH_METHOD (validated above). All non-
# UF2 paths need the sibling .hex — resolve it once here.
log "1/6 flashing (method=$FLASH_METHOD)..."
HEX="${UF2%.uf2}.hex"

# Flash helpers capture combined output rather than discarding it. The
# sub-scripts' diagnostics are the whole point on a failure — notably
# flash-swd.sh's #68 pre-flight guard ("another openocd already holds the
# SWD bus"), which is otherwise swallowed and leaves the operator staring
# at a bare "all enabled flash paths failed".
FLASH_LOG="$(mktemp -t test-join-flash)"
trap 'rm -f "$FLASH_LOG"' EXIT

emit_flash_log() {
    [ -s "$FLASH_LOG" ] || return 0
    while IFS= read -r line; do
        log "       | $line"
    done < "$FLASH_LOG"
}

flash_via_usb_msc() {
    "$REPO_ROOT/tools/flash.sh" "$UF2" >"$FLASH_LOG" 2>&1
}
flash_via_usb_serial() {
    [ -f "$HEX" ] && "$REPO_ROOT/tools/flash-serial.sh" "$HEX" >"$FLASH_LOG" 2>&1
}
flash_via_swd() {
    [ -f "$HEX" ] && "$REPO_ROOT/tools/flash-swd.sh" "$HEX" >"$FLASH_LOG" 2>&1
}

flashed=""
if [ "$FLASH_METHOD" = auto ] || [ "$FLASH_METHOD" = usb ]; then
    if flash_via_usb_msc; then
        flashed="flash.sh"
    else
        log "     flash.sh failed"
        emit_flash_log
        if [ ! -f "$HEX" ]; then
            log "     (skipping CDC-DFU fallback — no sibling .hex at $HEX)"
        elif flash_via_usb_serial; then
            flashed="flash-serial.sh"
        else
            log "     flash-serial.sh failed"
            emit_flash_log
        fi
    fi
fi

if [ -z "$flashed" ] && { [ "$FLASH_METHOD" = auto ] || [ "$FLASH_METHOD" = swd ]; }; then
    if [ ! -f "$HEX" ]; then
        log "error: no sibling .hex found for SWD flash (looked for $HEX)"
        exit 3
    fi
    if flash_via_swd; then
        flashed="flash-swd.sh"
    else
        log "     flash-swd.sh failed"
        emit_flash_log
    fi
fi

if [ -z "$flashed" ]; then
    log "error: all enabled flash paths failed (method=$FLASH_METHOD)"
    exit 3
fi
log "     $flashed succeeded"

# -- 2. wait for boot --
log "2/6 waiting 8 s for boot..."
sleep 8

# -- 3. drop Z2M's cached entry so the pass/fail signal is trustworthy --
#
# Without this the test cannot tell a fresh interview from Z2M's cache
# (#69, and #20 before it). Z2M keeps interview_state, configured_reportings
# AND the endpoint's cluster list keyed by IEEE, and a device-side factory
# reset does not invalidate any of it — so the old SUCCESSFUL lingers and
# the poll below would read it as a pass. Worse, distinguishing "stale
# SUCCESSFUL" from "fresh SUCCESSFUL" by state alone is ambiguous, because
# right after the long-press the cache legitimately still says SUCCESSFUL.
#
# Removing first makes the start state provable: the first poll must read
# NOT_JOINED, so a later SUCCESSFUL can only be this cycle's interview. It
# also forces Z2M to re-read the simple descriptor, which is the only way
# a cluster-list change shows up (the stale-cluster-list trap from #62).
#
# The remove is VERIFIED rather than assumed. z2m-cli already sends
# {"force":true}, but a forced remove can still no-op (broker hiccup, Z2M
# mid-restart, wrong friendly name), and the failure is silent. An
# unverified remove means every downstream result rests on an assumption
# that was never checked — so treat "still present" as a hard stop.
#
# Cost: the device's Z2M/HA history resets on every run. Acceptable for a
# test that already factory-resets the device.
#
# NOTE on ordering: permit_join stays OFF for this step, deliberately. The
# step-4 rule ("permit_join before the device-side reset") is about the
# long-press in step 5, not about this remove. Turning permit_join on any
# earlier would let the device re-associate and repopulate the very cache
# entry being cleared here, which defeats the provable start state.
log "3/6 removing Z2M's cached device entry (forces a genuine re-interview)..."

device_presence() {
    ssh "$PI_ALIAS" 'timeout 3 ~/z2m-cli sub bridge/devices -C 1 2>/dev/null' \
        | python3 -c "
import json, sys
target = '$XIAO_IEEE'
try:
    data = json.load(sys.stdin)
except Exception:
    print('NO_DATA'); sys.exit(0)
for d in data:
    if d.get('ieee_address', '').lower() == target:
        print('PRESENT'); break
else:
    print('GONE')
" 2>/dev/null || echo "QUERY_ERROR"
}

# Issue the remove and wait for it to show up in bridge/devices.
#
# Two attempts, 20 s each. Observed 2026-07-30: a remove issued ~8 s after
# an SWD flash silently did nothing — Z2M answered the request but the
# device was still listed minutes later, while the identical command sent
# by hand took effect in under 3 s. Z2M is evidently busy in that window.
# The remove is idempotent, so retrying is free; what must NOT happen is
# proceeding on an unverified assumption, which is the #69 false PASS.
remove_and_verify() {
    if ! ssh "$PI_ALIAS" "~/z2m-cli remove $XIAO_IEEE" >/dev/null 2>&1; then
        log "     warning: remove command exited non-zero (still verifying)"
    fi
    local _i
    for _i in $(seq 1 10); do
        sleep 2
        PRESENCE=$(device_presence)
        if [ "$PRESENCE" = GONE ]; then
            return 0
        fi
    done
    return 1
}

REMOVED=0
if remove_and_verify; then
    REMOVED=1
else
    log "     remove didn't take within 20 s (last read: $PRESENCE) — retrying once..."
    if remove_and_verify; then
        REMOVED=1
    fi
fi

if [ $REMOVED -eq 0 ]; then
    log "error: Z2M still lists $XIAO_IEEE after two removes (last read: $PRESENCE)."
    log "       Refusing to continue — with the cache intact a later SUCCESSFUL"
    log "       cannot be distinguished from the previous join's leftovers, which"
    log "       is the #69 false PASS this test exists to prevent."
    log "       Check by hand:"
    log "         ssh $PI_ALIAS '~/z2m-cli remove $XIAO_IEEE'"
    log "         ssh $PI_ALIAS 'timeout 3 ~/z2m-cli sub bridge/devices -C 1'"
    exit 3
fi
log "     confirmed gone from Z2M's device list"

# -- 4. permit_join ON *before* the factory reset --
log "4/6 permit_join true (must precede the reset — coordinator needs to accept the auto-steer)..."
ssh "$PI_ALIAS" '~/z2m-cli permit-join true' >/dev/null

# -- 5. factory reset (device reboots → FIRST_START → default handler auto-steers) --
log "5/6 factory reset (long-press ~4 s)..."
ssh "$PI_ALIAS" '~/xiao-long-press.sh'

# -- 6. watch for the interview to complete --
log "6/6 watching zigbee2mqtt/bridge/event (timeout ${INTERVIEW_TIMEOUT_S}s)..."
# Poll bridge/devices for our device's interview_state. Z2M's
# bridge/event API in 2.x doesn't emit a specific
# "device_interview_successful" event, but the interview_state field
# on the device object flips to SUCCESSFUL / FAILED when interview
# terminates, so we poll for that.
OUTCOME=""
JOINED_LOGGED=0
# Belt-and-braces on top of the step-3 verified remove: with the cache
# dropped, the first poll MUST read NOT_JOINED, so this can only fire if
# the entry came back. Kept as an assertion rather than dropped, because a
# false PASS here is what poisons every downstream bench result (#69).
SAW_PREINTERVIEW=0
POLL_ERRORS=0
CLUSTERS=""
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
        # Advertised input clusters, keyed BY ENDPOINT. A flat union hides
        # a cluster moving between endpoints — the union is unchanged but
        # Z2M/HA bindings break — so keep the endpoint association (#69).
        parts = []
        for ep_id, ep in sorted((d.get('endpoints') or {}).items(), key=lambda kv: str(kv[0])):
            ins = sorted(set((ep.get('clusters') or {}).get('input') or []))
            if ins:
                parts.append(f'{ep_id}:{\",\".join(ins)}')
        print(f'{state}|{completed}|{\";\".join(parts)}')
        break
else:
    print('NOT_JOINED')
" 2>/dev/null || echo "QUERY_ERROR")
    STATE_NAME=$(echo "$STATE" | cut -d'|' -f1)
    # -s so a delimiter-less line (NOT_JOINED / NO_DATA / QUERY_ERROR)
    # yields empty rather than echoing the whole line back as if it were
    # a cluster list — otherwise failures print "clusters: NOT_JOINED".
    CLUSTERS=$(echo "$STATE" | cut -s -d'|' -f3)

    case "$STATE_NAME" in
        SUCCESSFUL)
            if [ $SAW_PREINTERVIEW -eq 0 ]; then
                OUTCOME=STALE
            else
                OUTCOME=SUCCESS
            fi
            break
            ;;
        FAILED)
            OUTCOME=FAILED
            break
            ;;
        NOT_JOINED)
            # Genuine "coordinator has no such device" — the expected
            # start state, and real evidence no interview has happened yet.
            SAW_PREINTERVIEW=1
            ;;
        NO_DATA|QUERY_ERROR|UNKNOWN)
            # NO INFORMATION, not evidence. The ssh/timeout/json step
            # failed, or Z2M returned a device with no interview_state.
            # These deliberately do NOT satisfy the pre-interview
            # assertion: counting a failed poll as proof of "not yet
            # interviewed" is precisely how a stale SUCCESSFUL on a later
            # poll gets laundered into a PASS (#69).
            POLL_ERRORS=$((POLL_ERRORS + 1))
            ;;
        *)
            # A real in-progress interview state (PENDING, IN_PROGRESS, …):
            # the device is known to the coordinator but not yet done.
            SAW_PREINTERVIEW=1
            if [ $JOINED_LOGGED -eq 0 ]; then
                log "   device in Z2M (state=$STATE_NAME) — waiting for interview to complete"
                JOINED_LOGGED=1
            fi
            ;;
    esac
    sleep 3
done

# Disable permit_join no matter the outcome
ssh "$PI_ALIAS" '~/z2m-cli permit-join false' >/dev/null 2>&1 || true

if [ -z "$OUTCOME" ]; then
    OUTCOME=TIMEOUT
fi

log "final interview_state: $STATE"
if [ -n "$CLUSTERS" ]; then
    log "advertised input clusters (by endpoint): $CLUSTERS"
fi
if [ $POLL_ERRORS -gt 0 ]; then
    log "note: $POLL_ERRORS/$i poll(s) returned no usable data (ssh, timeout or JSON)"
fi

# Optional interface assertion. Set EXPECT_CLUSTERS to regression-test a
# firmware cluster-list change — Z2M serves the cluster list from its own
# database and will happily keep reporting the OLD one after a device-side
# factory reset (#69). See the header for the two accepted forms.
normalize_ep_map() {
    echo "$1" | tr -d '[:space:]' | tr ';' '\n' | while IFS=: read -r ep cl; do
        [ -n "$ep" ] || continue
        printf '%s:%s\n' "$ep" "$(echo "$cl" | tr ',' '\n' | sort -u | paste -sd, -)"
    done | sort | paste -sd';' -
}

if [ "$OUTCOME" = SUCCESS ] && [ -n "${EXPECT_CLUSTERS:-}" ]; then
    if echo "$EXPECT_CLUSTERS" | grep -q ':'; then
        MODE="per-endpoint"
        WANT=$(normalize_ep_map "$EXPECT_CLUSTERS")
        GOT=$(normalize_ep_map "$CLUSTERS")
    else
        MODE="flat"
        # The flat form only describes one endpoint's worth of interface,
        # so it is only a valid assertion if there IS one endpoint. Without
        # this check a second endpoint (or a cluster relocated onto one)
        # passes silently while Z2M/HA bindings break.
        EP_COUNT=$(echo "$CLUSTERS" | tr ';' '\n' | grep -c ':' || true)
        if [ "$EP_COUNT" -ne 1 ]; then
            log "FAIL — EXPECT_CLUSTERS was given in flat form, which describes a"
            log "       single endpoint, but the device advertised $EP_COUNT endpoint(s)."
            log "       actual: $CLUSTERS"
            log "       Switch to the per-endpoint form to pin this down, e.g.:"
            log "         EXPECT_CLUSTERS='10:genBasic,genIdentify,genPollCtrl,seMetering'"
            exit 4
        fi
        WANT=$(echo "$EXPECT_CLUSTERS" | tr -d '[:space:]' | tr ',' '\n' | sort -u | paste -sd, -)
        GOT=$(echo "$CLUSTERS" | cut -s -d: -f2- | tr ',' '\n' | sort -u | paste -sd, -)
    fi

    if [ "$WANT" != "$GOT" ]; then
        log "FAIL — cluster list mismatch ($MODE comparison)"
        log "  expected: $WANT"
        log "  actual:   $GOT"
        log "  If you just changed the firmware's cluster list, Z2M is serving"
        log "  cached endpoint data. Remove and re-pair:"
        log "    ssh $PI_ALIAS '~/z2m-cli remove $XIAO_IEEE'"
        exit 4
    fi
    log "cluster list matches EXPECT_CLUSTERS ($MODE comparison)"
fi

case "$OUTCOME" in
    SUCCESS)
        log "PASS — join + interview completed"
        exit 0
        ;;
    STALE)
        log "FAIL — SUCCESSFUL on the very first poll, despite the step-3"
        log "       cache removal having been VERIFIED. That means the entry"
        log "       came back between the verify and this read, so this is"
        log "       cached data from a previous join rather than the interview"
        log "       we just triggered. Check by hand:"
        log "         ssh $PI_ALIAS '~/z2m-cli remove $XIAO_IEEE'"
        exit 3
        ;;
    FAILED)
        log "FAIL — Z2M reported interview failure"
        exit 1
        ;;
    TIMEOUT)
        log "FAIL — no terminal event in ${INTERVIEW_TIMEOUT_S}s"
        if [ $POLL_ERRORS -gt 0 ]; then
            log "       ($POLL_ERRORS poll(s) failed outright — if that's most of them,"
            log "        suspect the ssh/MQTT path rather than the device)"
        fi
        exit 2
        ;;
esac

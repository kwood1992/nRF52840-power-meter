#!/bin/bash
# Host tests for tools/test-join.sh — no hardware, no Pi, no network.
#
# Fakes the whole rig: `ssh` is stubbed with a scripted queue of
# bridge/devices JSON responses, `sleep` is a no-op, and the flash
# sub-scripts are stubs. That lets the real script's decision logic run
# end-to-end against canned Z2M states.

set -u

SCRIPT_UNDER_TEST="${1:?usage: run-tests.sh /path/to/test-join.sh}"
WORK="$(mktemp -d -t test-join-tests)"
trap 'rm -rf "$WORK"' EXIT

FAKE_REPO="$WORK/repo"
FAKEBIN="$WORK/bin"
mkdir -p "$FAKE_REPO/tools" "$FAKEBIN"
cp "$SCRIPT_UNDER_TEST" "$FAKE_REPO/tools/test-join.sh"
chmod +x "$FAKE_REPO/tools/test-join.sh"
# sibling .hex must exist or the SWD path bails before reaching the flash call
touch "$WORK/fw.uf2" "$WORK/fw.hex"

for s in flash.sh flash-serial.sh flash-swd.sh; do
    printf '#!/bin/bash\nexit ${FAKE_FLASH_RC:-0}\n' > "$FAKE_REPO/tools/$s"
    chmod +x "$FAKE_REPO/tools/$s"
done

# no-op sleep so the 90 s poll window runs instantly
printf '#!/bin/bash\nexit 0\n' > "$FAKEBIN/sleep"
chmod +x "$FAKEBIN/sleep"

# ssh stub: serves $WORK/queue/NNN.json in order for bridge/devices polls,
# repeating the last entry once exhausted.
cat > "$FAKEBIN/ssh" <<'STUB'
#!/bin/bash
CMD="$2"
case "$CMD" in
    *XIAO_FRIENDLY_NAME*) echo "0x1234"; exit 0 ;;
    *"sub bridge/devices"*)
        N=$(cat "$QUEUE_DIR/.counter" 2>/dev/null || echo 0)
        N=$((N + 1)); echo "$N" > "$QUEUE_DIR/.counter"
        F="$QUEUE_DIR/$N.json"
        if [ ! -f "$F" ]; then
            LAST=$(ls "$QUEUE_DIR" 2>/dev/null | grep -o '^[0-9]\{1,\}' | sort -n | tail -1)
            F="$QUEUE_DIR/$LAST.json"
        fi
        [ -n "$F" ] && [ -f "$F" ] && cat "$F"
        exit 0
        ;;
    *) exit 0 ;;
esac
STUB
chmod +x "$FAKEBIN/ssh"

PASS=0; FAIL=0

# queue <json>... — each arg becomes one scripted bridge/devices response
queue() {
    rm -rf "$WORK/queue"; mkdir -p "$WORK/queue"
    local i=1
    for body in "$@"; do
        printf '%s' "$body" > "$WORK/queue/$i.json"
        i=$((i + 1))
    done
}

# expect <name> <expected-rc> [env assignments...] — runs the script
run_case() {
    local name="$1" want_rc="$2"; shift 2
    OUT=$(env PATH="$FAKEBIN:$PATH" QUEUE_DIR="$WORK/queue" "$@" \
        "$FAKE_REPO/tools/test-join.sh" "$WORK/fw.uf2" 2>&1)
    RC=$?
    if [ "$RC" -eq "$want_rc" ]; then
        echo "  PASS  $name (rc=$RC)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $name — wanted rc=$want_rc got rc=$RC"
        echo "$OUT" | sed 's/^/          /'
        FAIL=$((FAIL + 1))
    fi
}

assert_output_lacks() {
    if echo "$OUT" | grep -q "$1"; then
        echo "  FAIL  ...and output should not contain '$1'"
        echo "$OUT" | sed 's/^/          /'
        FAIL=$((FAIL + 1))
    else
        echo "  PASS  ...and output correctly lacks '$1'"
        PASS=$((PASS + 1))
    fi
}
assert_output_has() {
    if echo "$OUT" | grep -q "$1"; then
        echo "  PASS  ...and output contains '$1'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  ...and output should contain '$1'"
        echo "$OUT" | sed 's/^/          /'
        FAIL=$((FAIL + 1))
    fi
}

GONE='[]'
EP10='{"ieee_address":"0x1234","interview_state":"%s","interview_completed":%s,"endpoints":{"10":{"clusters":{"input":["genBasic","genIdentify","genPollCtrl","seMetering"]}}}}'
dev()  { printf "[$EP10]" "$1" "$2"; }
GARBAGE='not json at all'

echo "== #69 false-PASS regression: failed poll must not license a stale SUCCESSFUL =="
# verify-remove sees GONE, then the first real poll FAILS (no data), then a
# stale SUCCESSFUL appears. Pre-fix this was reported as PASS (rc=0).
queue "$GONE" "$GARBAGE" "$(dev SUCCESSFUL true)"
run_case "NO_DATA then SUCCESSFUL => STALE, not PASS" 3
assert_output_has "FAIL"

echo "== happy path =="
queue "$GONE" "$GONE" "$(dev PENDING false)" "$(dev SUCCESSFUL true)"
run_case "NOT_JOINED -> PENDING -> SUCCESSFUL => PASS" 0

echo "== unverified remove is now a hard stop =="
queue "$(dev SUCCESSFUL true)"
run_case "device never leaves Z2M's list => abort" 3
assert_output_has "still lists"

echo "== cut -s: delimiter-less states must not leak into the cluster line =="
queue "$GONE" "$GONE"
run_case "all polls NOT_JOINED => TIMEOUT" 2
assert_output_lacks "advertised input clusters"

echo "== EXPECT_CLUSTERS, flat form =="
queue "$GONE" "$GONE" "$(dev SUCCESSFUL true)"
run_case "flat form matches single endpoint" 0 \
    EXPECT_CLUSTERS=seMetering,genBasic,genPollCtrl,genIdentify

queue "$GONE" "$GONE" "$(dev SUCCESSFUL true)"
run_case "flat form detects a missing cluster" 4 \
    EXPECT_CLUSTERS=genBasic,genIdentify,genPollCtrl,seMetering,genPowerCfg

echo "== endpoint-level regression the old flat union could not see =="
TWO_EP='[{"ieee_address":"0x1234","interview_state":"SUCCESSFUL","interview_completed":true,"endpoints":{"10":{"clusters":{"input":["genBasic","genIdentify","genPollCtrl"]}},"11":{"clusters":{"input":["seMetering"]}}}}]'
queue "$GONE" "$GONE" "$TWO_EP"
run_case "seMetering moved to ep 11 => flat form refuses to pass" 4 \
    EXPECT_CLUSTERS=genBasic,genIdentify,genPollCtrl,seMetering

queue "$GONE" "$GONE" "$TWO_EP"
run_case "per-endpoint form catches the move" 4 \
    EXPECT_CLUSTERS='10:genBasic,genIdentify,genPollCtrl,seMetering'

queue "$GONE" "$GONE" "$(dev SUCCESSFUL true)"
run_case "per-endpoint form matches when correct" 0 \
    EXPECT_CLUSTERS='10:genBasic,genIdentify,genPollCtrl,seMetering'

echo "== flash diagnostics surface instead of being swallowed =="
printf '#!/bin/bash\necho "another openocd already holds the SWD bus" >&2\nexit 1\n' \
    > "$FAKE_REPO/tools/flash-swd.sh"
chmod +x "$FAKE_REPO/tools/flash-swd.sh"
queue "$GONE"
run_case "SWD pre-flight message reaches the operator" 3 FLASH_METHOD=swd FAKE_FLASH_RC=1
assert_output_has "another openocd already holds the SWD bus"

echo
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]

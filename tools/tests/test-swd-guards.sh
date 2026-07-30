#!/bin/bash
# Host tests for the #68 SWD-bus guards in flash-swd.sh / rtt-tail.sh.
# No Pi, no hardware: `ssh` and `scp` are stubbed and the Pi's openocd
# state is a directory of flag files.
#
# The case that matters is WEDGED: an openocd process that lost the telnet
# port bind. It holds the SWD bus but is invisible to `nc -z 4444`, so the
# old port-based detection reported the bus as free.

set -u

SRC_DIR="${1:?usage: run-swd-tests.sh <dir containing patched scripts + lib-swd.sh>}"
# Portable mktemp: BSD/macOS accepts `-d -t prefix`, but GNU coreutils
# treats the argument as a TEMPLATE and fails without trailing X's —
# leaving WORK empty, so every path below resolved against / and the
# suite failed on Linux only. Keep the explicit template form.
WORK="$(mktemp -d "${TMPDIR:-/tmp}/swd-guard-tests.XXXXXX")" || exit 1
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"; FAKEBIN="$WORK/bin"; STATE="$WORK/state"
mkdir -p "$REPO/tools" "$FAKEBIN" "$STATE"
for f in flash-swd.sh rtt-tail.sh lib-swd.sh; do
    cp "$SRC_DIR/$f" "$REPO/tools/$f"
done
chmod +x "$REPO/tools/flash-swd.sh" "$REPO/tools/rtt-tail.sh"

# A .hex that clears the app-slot floor: ELA base 0x30000, data at offset 0.
{ echo ":020000040003F7"; echo ":10000000$(printf '00%.0s' $(seq 16))F0"; echo ":00000001FF"; } \
    > "$WORK/app.hex"

printf '#!/bin/bash\nexit 0\n' > "$FAKEBIN/scp"; chmod +x "$FAKEBIN/scp"
printf '#!/bin/bash\nexit 0\n' > "$FAKEBIN/sleep"; chmod +x "$FAKEBIN/sleep"

cat > "$FAKEBIN/ssh" <<'STUB'
#!/bin/bash
CMD="$2"
case "$CMD" in
    *pgrep*)
        [ -f "$STATE_DIR/pids" ] && cat "$STATE_DIR/pids"
        exit 0 ;;
    *"echo shutdown"*)
        # openocd only honours telnet shutdown when its control port is live
        if [ -f "$STATE_DIR/telnet" ]; then
            rm -f "$STATE_DIR/pids" "$STATE_DIR/telnet"
        fi
        exit 0 ;;
    *"nc -z"*)
        [ -f "$STATE_DIR/telnet" ] && exit 0
        exit 1 ;;
    *) exit 0 ;;
esac
STUB
chmod +x "$FAKEBIN/ssh"

PASS=0; FAIL=0
set_state() { # set_state <pids|""> <telnet:yes|no>
    rm -f "$STATE/pids" "$STATE/telnet"
    [ -n "$1" ] && printf '%s\n' $1 > "$STATE/pids"
    [ "$2" = yes ] && touch "$STATE/telnet"
    return 0
}
run() { # run <name> <want_rc> <cmd...>
    local name="$1" want="$2"; shift 2
    OUT=$(env PATH="$FAKEBIN:$PATH" STATE_DIR="$STATE" "$@" 2>&1); RC=$?
    if [ "$RC" -eq "$want" ]; then echo "  PASS  $name (rc=$RC)"; PASS=$((PASS+1))
    else echo "  FAIL  $name — wanted rc=$want got rc=$RC"; echo "$OUT" | sed 's/^/          /'; FAIL=$((FAIL+1)); fi
}
has()  { if echo "$OUT" | grep -q "$1"; then echo "  PASS  ...says '$1'"; PASS=$((PASS+1));
         else echo "  FAIL  ...should say '$1'"; echo "$OUT" | sed 's/^/          /'; FAIL=$((FAIL+1)); fi; }
lacks(){ if echo "$OUT" | grep -q "$1"; then echo "  FAIL  ...must not say '$1'"; echo "$OUT" | sed 's/^/          /'; FAIL=$((FAIL+1));
         else echo "  PASS  ...correctly avoids '$1'"; PASS=$((PASS+1)); fi; }

RTT="$REPO/tools/rtt-tail.sh"
FLASH="$REPO/tools/flash-swd.sh"

echo "== rtt-tail --stop: bus already clear =="
set_state "" no
run "nothing to stop" 0 "$RTT" --stop
has "nothing to stop"

echo "== rtt-tail --stop: healthy openocd, telnet responsive =="
set_state "4321" yes
run "graceful shutdown succeeds" 0 "$RTT" --stop
has "0 processes remain"

echo "== #68 REGRESSION: wedged openocd — alive, telnet dead =="
set_state "4321" no
run "--stop must NOT claim the bus is free" 1 "$RTT" --stop
has "4321"
lacks "SWD bus released"
lacks "nothing to stop"

echo "== #68 REGRESSION: flash-swd must refuse a wedged openocd =="
set_state "4321" no
run "flash-swd refuses" 1 "$FLASH" "$WORK/app.hex"
has "hold the SWD bus"
has "cannot be shut down gracefully"

echo "== flash-swd: guard passes when the bus is genuinely free =="
set_state "" no
run "guard does not block" 1 "$FLASH" "$WORK/app.hex"   # rc=1 later, at 'Verified OK'
lacks "hold the SWD bus"
has "copying hex"

echo "== #68 REGRESSION: rtt-tail refuses to start a second openocd =="
set_state "4321" no
run "startup refuses on wedged bus" 1 "$RTT" -
has "Refusing to start a second one"

echo "== rtt-tail startup clears a healthy stale openocd and continues =="
set_state "4321" yes
run "stale instance cleared, startup proceeds" 1 "$RTT" -
lacks "Refusing to start a second one"

echo
echo "passed: $PASS   failed: $FAIL"
[ "$FAIL" -eq 0 ]

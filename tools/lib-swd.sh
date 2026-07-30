# shellcheck shell=bash
#
# Shared SWD-bus helpers for the Pi rig. Sourced by tools/flash-swd.sh and
# tools/rtt-tail.sh — not executable on its own, no shebang on purpose.
# The directive above names the dialect for shellcheck, which would
# otherwise have no shebang to infer it from (SC2148).
#
# Why this file exists (issue #68):
#   Both scripts drive openocd over the same two SWD wires, and a second
#   openocd on those wires corrupts an in-flight erase. The two had already
#   drifted: flash-swd.sh grew a pre-flight guard while rtt-tail.sh — the
#   script that actually leaks the process — had none. Keeping the detection
#   in one place is the point; don't re-inline it.
#
# Detection is by PROCESS, not by the telnet control port.
#
#   The control port only reveals the openocd that WON the bind. The #68
#   instance logged "couldn't bind gdb to socket on port 3333" — it lost the
#   race, held the bus anyway, and was completely invisible to `nc -z 4444`.
#   A port-based check in that state reports "bus released" while a second
#   master is still driving SWCLK/SWDIO, which is worse than no check at all
#   because the operator has been actively reassured.
#
#   `nc` being absent or renamed on the Pi produces the same false all-clear.
#
#   The port probe is kept, but only to answer "is it responsive?", which is
#   a different question from "is it there?".
#
# Callers must set PI_ALIAS and OPENOCD_TELNET_PORT before sourcing.

# PIDs of live openocd processes on the Pi, one per line (empty if none).
# `pgrep -x` matches the executable name exactly, so the `sudo openocd …`
# parent isn't double-counted (its comm is "sudo"). Readable without sudo.
pi_openocd_pids() {
    ssh "$PI_ALIAS" 'pgrep -x openocd 2>/dev/null || true' 2>/dev/null || true
}

pi_openocd_count() {
    pi_openocd_pids | grep -c . || true
}

# Something is listening on openocd's telnet control port. Says nothing
# about HOW MANY openocds exist — see the header note before using this
# as an all-clear signal.
pi_openocd_telnet_responsive() {
    ssh "$PI_ALIAS" "nc -z 127.0.0.1 $OPENOCD_TELNET_PORT >/dev/null 2>&1" >/dev/null 2>&1
}

# Graceful shutdown via openocd's own telnet command. Needs no sudo, which
# matters because passwordless sudo on the Pi is scoped to `openocd` only —
# `sudo pkill` would prompt for a password non-interactively and hang.
pi_stop_openocd() {
    ssh "$PI_ALIAS" \
        "echo shutdown | nc -q 1 127.0.0.1 $OPENOCD_TELNET_PORT 2>/dev/null || true" \
        >/dev/null 2>&1 || true
}

pi_openocd_manual_clear_advice() {
    echo "  Clear it interactively — needs your password, because passwordless" >&2
    echo "  sudo on the Pi is scoped to openocd only:" >&2
    echo "    ssh $PI_ALIAS 'sudo pkill -x openocd'" >&2
}

# Ask the surviving openocd(s) to shut down, then CONFIRM by process count.
# Returns 0 when the bus is provably free, 1 otherwise. Retries because
# openocd takes a moment to unwind after accepting `shutdown`.
pi_release_swd_bus() {
    local n
    n="$(pi_openocd_count)"
    if [ "$n" -eq 0 ]; then
        return 0
    fi

    pi_stop_openocd
    for _ in 1 2 3 4 5 6; do
        sleep 0.5
        n="$(pi_openocd_count)"
        if [ "$n" -eq 0 ]; then
            return 0
        fi
    done
    return 1
}

# Hard guard for anything about to touch flash. Refuses rather than guessing.
pi_assert_swd_bus_free() {
    local n
    n="$(pi_openocd_count)"
    if [ "$n" -eq 0 ]; then
        return 0
    fi

    echo "error: $n openocd process(es) already hold the SWD bus on $PI_ALIAS." >&2
    echo "  PIDs: $(pi_openocd_pids | tr '\n' ' ')" >&2
    echo "  Proceeding risks a partially-erased app slot (see issue #68)." >&2
    echo "" >&2
    echo "  Almost always a leaked tools/rtt-tail.sh. Clear it with:" >&2
    echo "    ./tools/rtt-tail.sh --stop" >&2
    echo "" >&2
    if ! pi_openocd_telnet_responsive; then
        echo "  NOTE: nothing is listening on telnet port $OPENOCD_TELNET_PORT, so this" >&2
        echo "  openocd cannot be shut down gracefully — it lost the port bind but" >&2
        echo "  still holds the bus. --stop will NOT clear it." >&2
        pi_openocd_manual_clear_advice
    fi
    return 1
}

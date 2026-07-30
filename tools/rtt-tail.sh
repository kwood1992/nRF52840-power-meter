#!/bin/bash
# Stream firmware logs from the XIAO nRF52840 over RTT-over-SWD (issue #51).
#
# Why this exists:
#   USB is unplugged on the INA219 current-measurement rig (dual-supply
#   hazard on the BAT rail — see flash-swd.sh header). CDC-ACM console
#   is therefore unreachable, so we can't see LOG_INF output during
#   battery-mode diagnosis. RTT rides the same 2-wire SWD interface the
#   Pi already uses for flashing, so no new wiring is needed.
#
# How it works:
#   1. Cleans up any stale openocd on the Pi via its telnet control port
#      (a previous rtt-tail that wasn't torn down cleanly would block
#      flash-swd.sh).
#   2. Starts openocd on the Pi bound to 127.0.0.1 with the target
#      HALTED after RTT setup + RTT server bind on port 9090. The
#      device is intentionally NOT released yet — openocd drops
#      polled RTT data when no client is connected, so any log lines
#      emitted before we attach would be lost. Holding reset until
#      the client is on the wire ensures boot logs make it out.
#   3. Opens an SSH tunnel Mac:9090 → Pi:127.0.0.1:9090.
#   4. Starts a local nc-tee background job on the tunneled port.
#   5. Once the client is confirmed connected, tells openocd (via its
#      Pi-local telnet control port 4444) to `reset run`. Boot logs
#      then flow through openocd → tunnel → nc → tee.
#   6. Waits on the nc-tee job. Ctrl+C tears everything down cleanly.
#
# The RTT control block is discovered by whole-RAM scan of the
# nRF52840's 256 KB SRAM (0x20000000 + 0x40000). No ELF upload
# to the Pi required.
#
# Firmware must be built with -DEXTRA_CONF_FILE=rtt.conf so
# CONFIG_LOG_BACKEND_RTT=y and the log subsystem is targeting
# the RTT up-buffer.
#
# Prerequisites: same rpi-xiao SSH alias + passwordless-sudo-openocd
# setup used by tools/flash-swd.sh. Teardown uses openocd's own
# `shutdown` command over the telnet control port so no additional
# sudo scope is needed.
#
# Usage:
#   ./tools/rtt-tail.sh                    # log lands in docs/working/rtt-<ts>-capture.log
#   ./tools/rtt-tail.sh baseline-post-join # ...rtt-<ts>-baseline-post-join.log
#   ./tools/rtt-tail.sh -                  # stream to stdout only, no log file
#   ./tools/rtt-tail.sh --stop             # clear a leaked openocd and exit
#
# LEAKED-OPENOCD WARNING (issue #68):
#   A leftover openocd holds the SWD bus, and flash-swd.sh starting a
#   second one mid-erase corrupted the app slot on 2026-07-30. The
#   cleanup trap below covers HUP as well as INT/TERM/EXIT precisely
#   because closing the terminal is the common way this leaks — an
#   untrapped fatal signal kills the shell without running EXIT.
#   If one does survive, `--stop` clears it without needing sudo.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PI_ALIAS="rpi-xiao"
RTT_PORT=9090
OPENOCD_TELNET_PORT=4444
LABEL="${1:-capture}"

# nRF52840 RAM footprint: 256 KB at 0x20000000. Whole-RAM scan for the
# "SEGGER RTT" marker — slower than a symbol-lookup but keeps this
# script from needing the ELF on the Pi.
RTT_RAM_ADDR="0x20000000"
RTT_RAM_SIZE="0x40000"

REMOTE_LOG="/tmp/openocd-rtt.log"
SSH_TUNNEL_PID=""
NC_PID=""

# SWD-bus detection and teardown live in the shared lib so this script
# and flash-swd.sh cannot diverge on the check that keeps two openocds
# off the same wires (issue #68). Detection is by process, not by
# control port — see tools/lib-swd.sh for why the port probe alone
# gives a false all-clear.
# shellcheck source=tools/lib-swd.sh
source "$REPO_ROOT/tools/lib-swd.sh"

# `--stop` exists so a leaked instance can be cleared without sudo:
# passwordless sudo on the Pi is scoped to `openocd` only, so
# `sudo pkill -f openocd` prompts for a password and can't be scripted.
# openocd's own telnet `shutdown` needs no elevation at all.
if [ "${1:-}" = "--stop" ]; then
    STOP_COUNT="$(pi_openocd_count)"
    if [ "$STOP_COUNT" -eq 0 ]; then
        echo "no openocd running on $PI_ALIAS — nothing to stop."
        exit 0
    fi
    echo "-> $STOP_COUNT openocd process(es) on $PI_ALIAS; sending shutdown..."
    if ! pi_release_swd_bus; then
        echo "error: $(pi_openocd_count) openocd process(es) survived the shutdown." >&2
        echo "  PIDs: $(pi_openocd_pids | tr '\n' ' ')" >&2
        echo "  An openocd that lost the telnet-port bind cannot be shut down" >&2
        echo "  gracefully, so this is the expected #68 failure mode." >&2
        pi_openocd_manual_clear_advice
        exit 1
    fi
    echo "openocd stopped — SWD bus released (0 processes remain)."
    exit 0
fi

# Log-file setup happens AFTER the --stop early-exit, so `--stop` doesn't
# create a stray capture log named after the flag.
LOG_DIR="$REPO_ROOT/seeed-studio-zigbee-energy-meter/docs/working"
if [ "$LABEL" = "-" ]; then
    LOG_PATH=""
    echo "streaming RTT to stdout only (no log file)" >&2
else
    TS="$(date +%Y-%m-%d-%H%M%S)"
    LOG_PATH="$LOG_DIR/rtt-$TS-$LABEL.log"
    mkdir -p "$LOG_DIR"
    echo "logging RTT stream to $LOG_PATH" >&2
fi

cleanup() {
    local rc=$?
    trap - EXIT INT TERM HUP
    echo >&2
    echo "-> tearing down rtt-tail session..." >&2
    if [ -n "$NC_PID" ] && kill -0 "$NC_PID" 2>/dev/null; then
        kill "$NC_PID" 2>/dev/null || true
    fi
    if [ -n "$SSH_TUNNEL_PID" ] && kill -0 "$SSH_TUNNEL_PID" 2>/dev/null; then
        kill "$SSH_TUNNEL_PID" 2>/dev/null || true
        wait "$SSH_TUNNEL_PID" 2>/dev/null || true
    fi
    pi_stop_openocd
    exit "$rc"
}
# HUP matters as much as INT/TERM here: closing the terminal or dropping
# the SSH session is the usual way this script's openocd got leaked
# (#68). Without HUP trapped, the shell dies on an untrapped fatal signal
# and the EXIT trap never fires.
trap cleanup EXIT INT TERM HUP

# Clear any stale openocd and PROVE it is gone before starting ours.
# The old code sent `shutdown`, slept 0.5 s and started a second
# openocd regardless — so a wedged instance (the #68 state, where the
# telnet port is unresponsive and `shutdown` is silently ignored) left
# two masters driving SWCLK/SWDIO. That is the bus fight that corrupted
# an in-flight erase; this script is the one that leaks the process, so
# it is the one that most needs the guard.
echo "-> stopping any stale openocd on $PI_ALIAS..."
if ! pi_release_swd_bus; then
    echo "error: $(pi_openocd_count) openocd process(es) still hold the SWD bus." >&2
    echo "  PIDs: $(pi_openocd_pids | tr '\n' ' ')" >&2
    echo "  Refusing to start a second one — see issue #68." >&2
    pi_openocd_manual_clear_advice
    exit 1
fi

# Same interface + target config as flash-swd.sh, plus RTT setup, and
# `reset halt` (not `reset run`) so the target stays paused until the
# client is on the wire.
#
# `bindto 127.0.0.1` keeps the RTT + telnet ports local to the Pi —
# the Mac reaches RTT through the SSH tunnel below; the telnet port
# stays Pi-local and is driven via ssh.
#
# Passwordless sudo on the Pi is scoped to `openocd` only (memory:
# reference_pi_passwordless_sudo_scope), so it's `sudo openocd …` —
# NOT `sudo nohup openocd …` (nohup isn't allowlisted). Redirect all
# stdio to files / /dev/null + `&` so ssh returns immediately without
# holding the openocd process's file descriptors open.
echo "-> starting openocd on $PI_ALIAS (target held HALTED until client attaches)..."
if ! ssh "$PI_ALIAS" "\
        sudo openocd \
            -c 'bindto 127.0.0.1' \
            -f interface/raspberrypi-swd.cfg \
            -c 'transport select swd' \
            -c 'adapter speed 1000' \
            -f target/nordic/nrf52.cfg \
            -c 'init; reset halt; rtt setup $RTT_RAM_ADDR $RTT_RAM_SIZE \"SEGGER RTT\"; rtt start; rtt server start $RTT_PORT 0' \
            </dev/null > $REMOTE_LOG 2>&1 & \
        for _ in \$(seq 1 40); do \
            if nc -z 127.0.0.1 $RTT_PORT 2>/dev/null && \
               nc -z 127.0.0.1 $OPENOCD_TELNET_PORT 2>/dev/null; then \
                echo READY; \
                exit 0; \
            fi; \
            sleep 0.25; \
        done; \
        echo NOT_READY; \
        exit 1" | grep -q READY; then
    echo "error: openocd never came up on $PI_ALIAS — log follows:" >&2
    ssh "$PI_ALIAS" "cat $REMOTE_LOG" >&2 || true
    exit 1
fi

# Local tunnel: Mac:$RTT_PORT → Pi:127.0.0.1:$RTT_PORT. -N = no
# remote command, -T = no PTY. ExitOnForwardFailure catches "local
# port already bound" instead of silently hanging.
echo "-> opening SSH tunnel localhost:$RTT_PORT → $PI_ALIAS:127.0.0.1:$RTT_PORT..."
ssh -N -T \
    -o ExitOnForwardFailure=yes \
    -o ServerAliveInterval=30 \
    -L "$RTT_PORT:127.0.0.1:$RTT_PORT" \
    "$PI_ALIAS" &
SSH_TUNNEL_PID=$!

# Wait for tunnel port to open locally.
for _ in $(seq 1 40); do
    if nc -z 127.0.0.1 "$RTT_PORT" >/dev/null 2>&1; then
        break
    fi
    if ! kill -0 "$SSH_TUNNEL_PID" 2>/dev/null; then
        echo "error: SSH tunnel died before becoming ready" >&2
        exit 1
    fi
    sleep 0.25
done

# Start the streaming nc + optional tee in the background. Doing this
# BEFORE `reset run` guarantees openocd has a live client to forward
# to — otherwise the boot-time flood of log messages would be polled
# and dropped by openocd before we're on the wire.
#
# `nc -d` = detach from stdin. Without it, BSD nc (macOS default)
# reads its stdin and — when the parent shell's stdin is /dev/null
# (invoked non-interactively, e.g. from another script) — sees EOF
# immediately and shuts down the socket. openocd then drops the RTT
# client and no data ever reaches the log file. -d makes nc a
# read-only sink for the socket, which is exactly what we need.
echo "-> streaming RTT (Ctrl+C to stop)"
echo "----"
if [ -n "$LOG_PATH" ]; then
    nc -d 127.0.0.1 "$RTT_PORT" | tee "$LOG_PATH" &
    NC_PID=$!
else
    nc -d 127.0.0.1 "$RTT_PORT" &
    NC_PID=$!
fi

# Give nc a beat to actually complete its TCP connect before we tell
# openocd to release the CPU.
sleep 0.5

echo "-> releasing target (reset run)..." >&2
ssh "$PI_ALIAS" \
    "echo 'reset run' | nc -q 1 127.0.0.1 $OPENOCD_TELNET_PORT >/dev/null 2>&1 || true"

# Block on nc — Ctrl+C propagates via the trap.
wait "$NC_PID"

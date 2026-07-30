#!/bin/bash
# Pi-scriptable POR (power-on-reset) of the XIAO via the NC relay in the
# Pi 3V3 → XIAO BAT power path (#8).
#
# Why this exists:
#   Pulling SWD wires does NOT clear the CDBGPWRUP latch in the ARM DP
#   CTRL/STAT register — that requires Vdd cycling (or an explicit DAP
#   write). Without a POR, INA219 measurements are contaminated by the
#   ~1.5 mA CoreSight/HFCLK overhead the debug interface holds. See
#   docs/working/2026-07-29-por-test-result.md for the full derivation.
#
#   Note the latch is cleared by Vdd cycling whether or not the SWD
#   jumpers are physically connected — leaving them wired is fine, as
#   long as no openocd is *attached* during the measurement.
#
# Wiring (per reference_pi_rig_gpio_map memory):
#   Pi 3V3 rail → relay NC contact → COM → XIAO BAT pad
#   Relay coil driven from Pi BCM 22 (physical pin 15) via a driver
#   (relay module's internal MOSFET or an external one — Pi GPIO alone
#   cannot source enough current for most coils; if you hear no click,
#   the coil isn't getting enough drive)
#
# Confirmed polarity (2026-07-29):
#   pinctrl set 22 op dh  → GPIO HIGH → coil energizes → NC opens → CUT
#   pinctrl set 22 dl     → GPIO LOW  → coil de-energizes → NC closes → ON
#
# THE POR IS NOT RELIABLE — THAT IS WHY THIS SCRIPT VERIFIES (2026-07-30):
#   Same script, same wiring, back-to-back runs produced 1.512 mA (latch
#   NOT cleared) and then −0.097 mA (cleared). A relay pulse that's too
#   short, or residual rail charge in the decoupling caps, leaves the
#   power domain up and the latch set. The old version of this script
#   reported success either way, and two full captures (360 s + 150 s)
#   were burned on a contaminated baseline before anyone noticed — a bad
#   run is indistinguishable from a good one at the summary line.
#
#   So: pulse, let the device settle, take a short INA219 sample, and
#   classify it. Retry the POR automatically if it didn't take.
#
# Usage:
#   ./tools/xiao-por.sh                 # POR + verify (default)
#   ./tools/xiao-por.sh 5               # 5 s cut window instead of 3 s
#   ./tools/xiao-por.sh --no-verify     # just pulse, skip the check
#   ./tools/xiao-por.sh --attempts 4    # more POR retries (default 3)
#
# Exit codes:
#   0  POR verified clean
#   1  rig/usage error
#   2  POR did not take after every attempt (baseline is contaminated)
#   3  couldn't classify — device never went idle enough to judge

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PI_ALIAS="rpi-xiao"
INA_PY="$REPO_ROOT/tools/ina219-sample.py"

CUT_S=3
VERIFY=1
ATTEMPTS=3
SETTLE_S=15
SAMPLE_S=10
SAMPLE_HZ=10

while [ $# -gt 0 ]; do
    case "$1" in
        --no-verify) VERIFY=0; shift ;;
        --attempts)  ATTEMPTS="${2:?--attempts needs a value}"; shift 2 ;;
        --settle)    SETTLE_S="${2:?--settle needs a value}"; shift 2 ;;
        --sample)    SAMPLE_S="${2:?--sample needs a value}"; shift 2 ;;
        -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
        -*)          echo "error: unknown flag $1" >&2; exit 1 ;;
        *)           CUT_S="$1"; shift ;;
    esac
done

# Classification thresholds, in mA. Anchored on real captures in
# docs/working/measurements/:
#   failed POR  -> p50 1.500 (144349-postpor-idle, 145209-pulse-burst)
#   clean       -> p50 -0.100 (150812-clean-idle-360)
#   rejoin scan -> mean 8.6-8.7, bimodal
#
# The decision is made on the MEDIAN alone, deliberately. An earlier
# version also required a small sd for "contaminated", which misfired on
# real data: 144349-postpor-idle is unambiguously contaminated (p50 1.500,
# p95 1.700) but has sd 0.588 because radio-TX spikes reach 29.8 mA. Those
# spikes are legitimate traffic and say nothing about the CoreSight latch.
# The median ignores them, which is exactly why it's the right statistic.
CLEAN_MAX_P50=0.5      # below this, the latch is clear
CONTAM_LO_P50=1.0      # \ the ~1.5 mA CoreSight plateau
CONTAM_HI_P50=2.2      # /

por_pulse() {
    ssh "$PI_ALIAS" \
        "pinctrl set 22 op dh; sleep ${CUT_S}; pinctrl set 22 dl; sleep 0.05; pinctrl set 22 ip pu"
}

# Echoes "p50 sd mean" from a short INA219 capture, or nothing on failure.
# Streams the sampler over SSH the same way measure-power.sh does, so
# nothing needs to be installed on the Pi.
sample_stats() {
    ssh "$PI_ALIAS" "python3 - --hz $SAMPLE_HZ --duration $SAMPLE_S --note por-verify" \
        < "$INA_PY" 2>&1 >/dev/null \
        | sed -n 's/^# summary .*mean=\([-0-9.]*\)mA sd=\([-0-9.]*\)mA .*p50=\([-0-9.]*\)mA.*/\3 \2 \1/p'
}

# clean | contaminated | busy | unknown
classify() {
    python3 -c "
import sys
p50, sd, mean = (float(x) for x in sys.argv[1:4])
if p50 < $CLEAN_MAX_P50:
    print('clean')
elif $CONTAM_LO_P50 <= p50 <= $CONTAM_HI_P50:
    print('contaminated')
elif p50 > $CONTAM_HI_P50:
    print('busy')
else:
    print('unknown')
" "$1" "$2" "$3"
}

if [ "$VERIFY" -eq 0 ]; then
    por_pulse
    echo "POR done — XIAO booted, ~2 s to firmware-live (NOT verified: --no-verify)"
    echo "  warning: an unverified POR fails silently ~half the time. Any capture" >&2
    echo "  taken after this may carry the ~1.5 mA CoreSight offset." >&2
    exit 0
fi

if [ ! -f "$INA_PY" ]; then
    echo "error: $INA_PY not found — needed to verify the POR" >&2
    exit 1
fi

for attempt in $(seq 1 "$ATTEMPTS"); do
    echo "-> POR attempt $attempt/$ATTEMPTS (${CUT_S}s cut)..."
    por_pulse
    echo "   settling ${SETTLE_S}s, then sampling ${SAMPLE_S}s @ ${SAMPLE_HZ}Hz..."
    sleep "$SETTLE_S"

    STATS="$(sample_stats || true)"
    if [ -z "$STATS" ]; then
        echo "   warning: no INA219 summary returned (I2C busy or sampler failed)" >&2
        continue
    fi
    # shellcheck disable=SC2086
    set -- $STATS
    P50="$1"; SD="$2"; MEAN="$3"
    VERDICT="$(classify "$P50" "$SD" "$MEAN")"
    echo "   p50=${P50}mA sd=${SD}mA mean=${MEAN}mA -> $VERDICT"

    case "$VERDICT" in
        clean)
            echo "POR verified — CoreSight latch cleared, baseline is trustworthy."
            exit 0
            ;;
        contaminated)
            echo "   POR did NOT take (the ~1.5 mA CoreSight plateau is still there)."
            ;;
        busy)
            # Device is scanning/rejoining and swamps the signal, so the
            # latch state can't be judged. Not a POR failure by itself.
            echo "   device is busy (scan/rejoin) — can't judge the latch yet; waiting 30 s"
            sleep 30
            STATS="$(sample_stats || true)"
            if [ -n "$STATS" ]; then
                # shellcheck disable=SC2086
                set -- $STATS
                VERDICT="$(classify "$1" "$2" "$3")"
                echo "   re-sampled: p50=${1}mA sd=${2}mA mean=${3}mA -> $VERDICT"
                if [ "$VERDICT" = clean ]; then
                    echo "POR verified — CoreSight latch cleared, baseline is trustworthy."
                    exit 0
                fi
            fi
            ;;
        unknown)
            echo "   inconclusive — p50 sits between the clean and contaminated bands."
            ;;
    esac
done

if [ "${VERDICT:-}" = contaminated ]; then
    echo "error: POR failed to clear the CoreSight latch after $ATTEMPTS attempts." >&2
    echo "  The ~1.5 mA plateau means any capture taken now is contaminated and" >&2
    echo "  must NOT be used for battery-life numbers." >&2
    echo "  Try a longer cut window — the rail may not be collapsing fully:" >&2
    echo "    ./tools/xiao-por.sh 8" >&2
    exit 2
fi

echo "error: could not verify the POR after $ATTEMPTS attempts (last: ${VERDICT:-no-data})." >&2
echo "  The device never settled into an idle state clean enough to judge." >&2
echo "  Check it's joined and not stuck scanning, then retry with a longer settle:" >&2
echo "    ./tools/xiao-por.sh --settle 60" >&2
exit 3

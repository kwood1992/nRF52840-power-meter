#!/bin/bash
# Bench pulse-simulator for the XIAO nRF52840 pulse-counter (#16).
#
# Pulses Pi BCM GPIO 27 LOW for ~250 ms, which the XIAO sees as one
# falling edge on D7 (P0.04) and increments bench_pulse_count by 1.
# No side effects: the D7 ISR is dedicated to pulse counting, so no
# join callback fires and no debounce logic drops fast bursts.
#
# Install: copy this to ~/xiao-pulse.sh on the Pi and `chmod +x`.
#
# See docs/swd-recovery-jig.md ("Optional bench-input wires") for the
# Pi ↔ XIAO wire mapping.

set -euo pipefail

PIN=27
LOW_MS="${1:-250}"

pinctrl set "$PIN" op dh    # drive high (idle)
pinctrl set "$PIN" dl       # drive low  (pulse start)
sleep "$(awk "BEGIN {print $LOW_MS/1000}")"
pinctrl set "$PIN" dh       # release to high (pulse end)
pinctrl set "$PIN" ip pu    # back to pulled-up input so we're not sourcing current

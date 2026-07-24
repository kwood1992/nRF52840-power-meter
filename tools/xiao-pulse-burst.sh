#!/bin/bash
# Fire N bench pulses at the XIAO over the D7 pulse-input wire (#16).
#
# Usage:
#   ~/xiao-pulse-burst.sh <count> [gap_ms]
#
#   count   — number of pulses to send (required)
#   gap_ms  — idle time between pulses in ms (default 100)
#
# The per-pulse LOW time comes from ~/xiao-pulse.sh (default 250 ms), so
# a full pulse period is ~350 ms with defaults — comfortably slow enough
# that even a Zigbee-busy XIAO catches every edge.

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <count> [gap_ms]" >&2
    exit 1
fi

COUNT="$1"
GAP_MS="${2:-100}"
GAP_S="$(awk "BEGIN {print $GAP_MS/1000}")"

for ((i = 1; i <= COUNT; i++)); do
    ~/xiao-pulse.sh
    if [ "$i" -lt "$COUNT" ]; then
        sleep "$GAP_S"
    fi
done

echo "sent $COUNT pulse(s) on GPIO 27 -> XIAO D7"

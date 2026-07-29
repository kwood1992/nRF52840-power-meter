#!/usr/bin/env bash
# µs-precision pulse burst on the Pi's BCM 27 → XIAO D7 wire (#59).
#
# Exercises the min-pulse-width filter at threshold ± 100 µs, which the
# shell-driven `xiao-pulse.sh` can't reach (pinctrl fork/exec bottoms out
# at 5–15 ms per edge). Uses pigpio wave-DMA on the Pi for sub-µs edge
# accuracy.
#
# Usage: xiao-pulse-us.sh <count> <pulse_us> [gap_us=5000]
#
#   count     — number of pulses (1..65535)
#   pulse_us  — LOW pulse width in microseconds
#   gap_us    — HIGH gap between pulses in microseconds (default 5000)
#
# Prerequisites on the Pi:
#   sudo apt install python3-pigpio
#   sudo systemctl enable --now pigpiod
set -euo pipefail

if [[ $# -lt 2 ]]; then
  echo "usage: $0 <count> <pulse_us> [gap_us=5000]" >&2
  exit 2
fi

count="$1"
pulse_us="$2"
gap_us="${3:-5000}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
py_path="$repo_root/tools/xiao-pulse-us.py"

# shellcheck disable=SC2029  # remote-side expansion of these args is intentional
ssh rpi-xiao "python3 - --count ${count} --pulse-us ${pulse_us} --gap-us ${gap_us}" \
    < "$py_path"

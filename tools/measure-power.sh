#!/usr/bin/env bash
# Orchestrate an INA219 current + Z2M event capture over the same window,
# then plot both aligned on wall-clock (#35).
#
# Wraps three existing pieces:
#   - tools/ina219-sample.py   (Pi-side, streamed via SSH; already used by
#                               tools/ina219-sample.sh)
#   - tools/z2m-events.py      (Pi-side, streamed via SSH; taps ~/z2m-cli sub)
#   - tools/plot-power.py      (Mac-side, matplotlib)
#
# Both Pi-side scripts run in parallel over separate SSH sessions with
# `python3 -` reading them from stdin — nothing needs to live on the Pi
# ahead of time. Output CSVs + PNG land in docs/working/measurements/
# under a per-run timestamped directory.
#
# Usage:
#   measure-power.sh <label> [seconds=60] [hz=10]
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <label> [seconds=60] [hz=10]" >&2
  exit 2
fi

label="$1"
seconds="${2:-60}"
hz="${3:-10}"
pi_alias="${PI_ALIAS:-rpi-xiao}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
ina_py="$repo_root/tools/ina219-sample.py"
evt_py="$repo_root/tools/z2m-events.py"
plot_py="$repo_root/tools/plot-power.py"

stamp="$(date +%Y-%m-%d-%H%M%S)"
out_dir="$repo_root/seeed-studio-zigbee-energy-meter/docs/working/measurements/${stamp}-${label}"
mkdir -p "$out_dir"

fw_commit="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
fw_dirty=""
if ! git -C "$repo_root" diff --quiet 2>/dev/null; then
  fw_dirty="+dirty"
fi
note="label=${label} fw=${fw_commit}${fw_dirty}"

# The device's IEEE lives in the Pi's creds file (same source test-join.sh uses).
xiao_ieee="$(ssh "$pi_alias" 'grep XIAO_FRIENDLY_NAME ~/.mosquitto-xiao-creds | cut -d= -f2')"
if [[ -z "$xiao_ieee" ]]; then
  echo "error: could not read XIAO_FRIENDLY_NAME from Pi creds file" >&2
  exit 3
fi

ina_csv="$out_dir/ina219.csv"
evt_csv="$out_dir/events.csv"
plot_png="$out_dir/plot.png"

echo "measuring: label=${label} duration=${seconds}s rate=${hz}Hz ieee=${xiao_ieee}"
echo "output:    ${out_dir}/"

# Start the event tagger in the background; it'll self-terminate at duration + 5 s
# guard (in case the INA sampler slips) via z2m-events.py's --duration.
# shellcheck disable=SC2029  # remote expansion intentional
ssh "$pi_alias" \
  "python3 - --ieee '${xiao_ieee}' --duration $((seconds + 5)) --note '${note}'" \
  < "$evt_py" \
  > "$evt_csv" \
  2>/dev/null &
evt_pid=$!

# Foreground: run the INA sampler for its full window.
# shellcheck disable=SC2029
ssh "$pi_alias" \
  "python3 - --hz ${hz} --duration ${seconds} --note '${note}'" \
  < "$ina_py" \
  > "$ina_csv" \
  2> >(tee /dev/stderr | grep '^# summary' >> "$ina_csv")

# Give the tagger a beat to finish its final flush + exit its --duration timer.
wait "$evt_pid" 2>/dev/null || true

# Plot on the Mac — matplotlib install is a one-time `pip3 install matplotlib`.
python3 "$plot_py" "$ina_csv" --events "$evt_csv" --out "$plot_png"

echo "done: ${plot_png}"

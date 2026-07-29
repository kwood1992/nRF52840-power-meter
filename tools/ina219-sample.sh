#!/usr/bin/env bash
# Sample the INA219 on rpi-xiao and save a timestamped CSV under docs/working/.
# Usage: ina219-sample.sh <label> [seconds] [hz]
#   label     — short slug for the file name and the CSV header note
#   seconds   — default 60
#   hz        — default 10 (I2C1 headroom is ~200 Hz; keep well below that)
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 <label> [seconds=60] [hz=10]" >&2
  exit 2
fi

label="$1"
seconds="${2:-60}"
hz="${3:-10}"

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
py_path="$repo_root/tools/ina219-sample.py"
out_dir="$repo_root/seeed-studio-zigbee-energy-meter/docs/working"
mkdir -p "$out_dir"

# Firmware commit for provenance in the CSV header note.
fw_commit="$(git -C "$repo_root" rev-parse --short HEAD 2>/dev/null || echo unknown)"
fw_dirty=""
if ! git -C "$repo_root" diff --quiet 2>/dev/null; then
  fw_dirty="+dirty"
fi

stamp="$(date +%Y-%m-%d-%H%M%S)"
out_csv="$out_dir/ina219-${stamp}-${label}.csv"

note="label=${label} fw=${fw_commit}${fw_dirty}"

echo "sampling: label=${label} duration=${seconds}s rate=${hz}Hz fw=${fw_commit}${fw_dirty}"
echo "output:   ${out_csv}"

# shellcheck disable=SC2029  # remote-side expansion of these args is intentional
ssh rpi-xiao "python3 - --hz ${hz} --duration ${seconds} --note '${note}'" \
    < "$py_path" \
    > "$out_csv" \
    2> >(tee /dev/stderr | grep '^# summary' >> "$out_csv")

echo "done. summary line appended to CSV; open ${out_csv} to review."

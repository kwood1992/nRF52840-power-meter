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
# Usage:
#   ./tools/xiao-por.sh           # 3 s cut window (default, safe)
#   ./tools/xiao-por.sh 5         # custom cut duration in seconds
#
# The 3 s default is enough for Vdd to drop below the nRF52840's
# power-loss threshold and clear DP CTRL/STAT reliably. Longer is
# harmless but wastes time.

set -euo pipefail

CUT_S="${1:-3}"

ssh rpi-xiao "pinctrl set 22 op dh; sleep ${CUT_S}; pinctrl set 22 dl; sleep 0.05; pinctrl set 22 ip pu"

echo "POR done — XIAO booted, ~2 s to firmware-live"

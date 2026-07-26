# tools/

Bench helpers for flashing and exercising the XIAO nRF52840 build.

## Flashing

Three scripts, all take an optional path argument (default is the standard build output). Pick the one that matches how the board is powered.

| Script | Use when | Path | Powered from |
|---|---|---|---|
| `flash.sh` | Normal iteration | Pi triggers double-tap reset → macOS mounts XIAO-SENSE → `cp` UF2 | USB (Mac → XIAO) |
| `flash-serial.sh` | USB works but the bootloader's mass-storage endpoint is silent (Sequoia lock quirk, MSC glitch) | Pi triggers double-tap reset → `adafruit-nrfutil dfu serial` over CDC-ACM | USB (Mac → XIAO) |
| `flash-swd.sh` | USB is unplugged (current-measurement runs, enclosed board) | Pi's OpenOCD writes flash directly over SWD; no USB touched | External (Pi 3V3, PSU, batteries) |

All three depend on the same Pi setup: `rpi-xiao` SSH alias with key auth, `~/xiao-bootloader.sh` on the Pi, SWD wires from Pi GPIO 24/25 → XIAO SWDIO/SWCLK, and passwordless `sudo openocd` (SWD-only path).

Do **not** leave USB plugged into the XIAO while the board is also powered from the Pi 3V3 → BAT rail. Two supplies through different regulators in parallel is not a safe steady state. `flash-swd.sh` explicitly targets that off-USB scenario.

## Other helpers

- `test-join.sh` — force-rejoin cycle: kick the device out via Z2M, watch it re-interview, PASS/FAIL on the resulting interview_state. See known caveat about stale `interview_state` from a prior join reporting SUCCESSFUL falsely.
- `xiao-pulse.sh` / `xiao-pulse-burst.sh` — drive the D7 pulse-simulator GPIO on the Pi to feed synthetic pulses into the meter.

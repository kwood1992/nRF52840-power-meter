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

## Debugging without USB

On the INA219 current-measurement rig the XIAO's USB is unplugged (dual-supply hazard on the BAT rail — see `flash-swd.sh` header). CDC-ACM console is not reachable, so `LOG_INF` output would otherwise be invisible. `rtt-tail.sh` streams the firmware log over the same 2-wire SWD interface the Pi already uses for flashing.

Firmware side: build with the `rtt.conf` overlay so `CONFIG_LOG_BACKEND_RTT=y` and the UART log backend is disabled:

```
west build -p auto -b xiao_ble/nrf52840 -- -DEXTRA_CONF_FILE=rtt.conf
./tools/flash-swd.sh
```

Host side:

```
./tools/rtt-tail.sh [label]   # log lands at seeed-studio-zigbee-energy-meter/docs/working/rtt-<ts>-<label>.log
./tools/rtt-tail.sh -         # stream to stdout only, no log file
```

Ctrl+C tears the OpenOCD session down cleanly so the next `flash-swd.sh` / `test-join.sh` isn't blocked by a stale RTT server holding the SWD wires. If a previous run wasn't cleanly interrupted, `rtt-tail.sh` kills any stale openocd on the Pi at startup, so it's safe to just re-run.

Don't build the USB-dev workflow with `rtt.conf` — it turns off CDC-ACM console, which is the whole point of USB iteration.

## Other helpers

- `test-join.sh` — force-rejoin cycle: kick the device out via Z2M, watch it re-interview, PASS/FAIL on the resulting interview_state. See known caveat about stale `interview_state` from a prior join reporting SUCCESSFUL falsely.
- `xiao-pulse.sh` / `xiao-pulse-burst.sh` — drive the D7 pulse-simulator GPIO on the Pi to feed synthetic pulses into the meter.

## Current measurement

- `ina219-sample.sh` / `ina219-sample.py` — one-shot INA219 CSV over the shunt in the Pi 3V3 → XIAO BAT path. See `docs/working/` for captured baselines.
- `xiao-por.sh` — pulse the BCM 22 relay in the 3V3 rail to force a full POR (clears the CoreSight `CDBGPWRUP` latch that inflates baselines by ~1.5 mA otherwise).
- `measure-power.sh <label> [s] [hz]` — orchestrator for #35: runs `ina219-sample.py` and `z2m-events.py` in parallel over SSH, produces `ina219.csv` + `events.csv` in `docs/working/measurements/<ts>-<label>/`, and renders `plot.png` with current vs. wall-clock annotated by Z2M events. Requires `pip3 install matplotlib` on the Mac. Pi-side scripts stream over SSH — no install on the Pi ahead of time.

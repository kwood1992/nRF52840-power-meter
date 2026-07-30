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
./tools/rtt-tail.sh --stop    # clear a leaked openocd and exit
```

Ctrl+C tears the OpenOCD session down cleanly, and the trap covers `HUP` as well as `INT`/`TERM`/`EXIT` so closing the terminal doesn't leak the process either. `rtt-tail.sh` also stops any stale openocd on the Pi at startup, so re-running is safe.

### If a flash fails with parity / NVMC errors, check for a leaked openocd

A second openocd on the same SWD wires is **not** a benign "port in use" problem — two of them fighting over SWCLK/SWDIO can fail an erase partway and leave the app slot partially erased, so the board comes back running nothing. This happened on 2026-07-30 (issue #68).

`flash-swd.sh` now pre-flights for this and refuses *before* touching flash. If it does:

```
./tools/rtt-tail.sh --stop                 # normal fix, needs no sudo
ssh rpi-xiao 'sudo pkill -f openocd'       # only if the telnet port is wedged
```

The second form needs your password — passwordless sudo on the Pi is scoped to `openocd` only. To check by hand:

```
ssh rpi-xiao 'ps -eo pid,user,etime,cmd | grep [o]penocd'
```

**Never** run the `pinctrl set 24 ip pd` SWD continuity probe while an openocd is running. Its bitbang driver sets pin direction once at init and never re-asserts it, so flipping GPIO 24/25 underneath it wedges that instance into `Error connecting DP: cannot read IDR` permanently — a power cycle does not recover it, only restarting openocd does.

Don't build the USB-dev workflow with `rtt.conf` — it turns off CDC-ACM console, which is the whole point of USB iteration.

## Other helpers

- `test-join.sh` — force-rejoin cycle: remove Z2M's cached entry, factory-reset the device, watch it re-interview, PASS/FAIL on the result.

  It removes the Z2M device entry first (step 3) because Z2M keys `interview_state`, `configured_reportings` **and the endpoint's cluster list** by IEEE, and a device-side factory reset invalidates none of it. Without the removal the test can pass off the previous join's cache, and can report a stale cluster list as current — both observed. The cost is that the device's Z2M/HA history resets on every run.

  It prints the advertised input clusters on completion, and `EXPECT_CLUSTERS` asserts them (order-insensitive, exit 4 on mismatch) so a firmware cluster-list change can be regression-tested:

  ```
  EXPECT_CLUSTERS=genBasic,genIdentify,genPollCtrl,seMetering ./tools/test-join.sh
  ```
- `xiao-pulse.sh` / `xiao-pulse-burst.sh` — drive the D7 pulse-simulator GPIO on the Pi to feed synthetic pulses into the meter. Shell + `pinctrl`, so per-edge timing bottoms out at 5–15 ms.
- `xiao-pulse-us.sh` — µs-precision variant for #59's min-pulse-width filter AC test (threshold ± 100 µs boundary discrimination). Writes BCM `GPSET0` / `GPCLR0` directly through `/dev/gpiomem` and busy-waits on `perf_counter_ns`, so sub-millisecond pulse widths are actually accurate. No daemon dependency — user in the `gpio` group runs it without sudo.

## Current measurement

- `ina219-sample.sh` / `ina219-sample.py` — one-shot INA219 CSV over the shunt in the Pi 3V3 → XIAO BAT path. See `docs/working/` for captured baselines.
- `xiao-por.sh` — pulse the BCM 22 relay in the 3V3 rail to force a full POR (clears the CoreSight `CDBGPWRUP` latch that inflates baselines by ~1.5 mA otherwise).
- `measure-power.sh <label> [s] [hz]` — orchestrator for #35: runs `ina219-sample.py` and `z2m-events.py` in parallel over SSH, produces `ina219.csv` + `events.csv` in `docs/working/measurements/<ts>-<label>/`, and renders `plot.png` with current vs. wall-clock annotated by Z2M events. Requires `pip3 install matplotlib` on the Mac. Pi-side scripts stream over SSH — no install on the Pi ahead of time.

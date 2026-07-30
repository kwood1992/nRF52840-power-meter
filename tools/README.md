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

Ctrl+C tears the OpenOCD session down cleanly, and the trap covers `HUP` as well as `INT`/`TERM`/`EXIT` so closing the terminal doesn't leak the process either. `rtt-tail.sh` also stops any stale openocd on the Pi at startup — and then *confirms* it's gone before starting its own, refusing rather than adding a second master to the bus if one survives.

### If a flash fails with parity / NVMC errors, check for a leaked openocd

A second openocd on the same SWD wires is **not** a benign "port in use" problem — two of them fighting over SWCLK/SWDIO can fail an erase partway and leave the app slot partially erased, so the board comes back running nothing. This happened on 2026-07-30 (issue #68).

`flash-swd.sh` and `rtt-tail.sh` both pre-flight for this and refuse *before* touching flash. If one does:

```
./tools/rtt-tail.sh --stop                 # normal fix, needs no sudo
ssh rpi-xiao 'sudo pkill -x openocd'       # only if the telnet port is wedged
```

The second form needs your password — passwordless sudo on the Pi is scoped to `openocd` only. To check by hand:

```
ssh rpi-xiao 'pgrep -x openocd'
```

Detection counts openocd **processes**, not listeners on the telnet control port. An openocd that loses the port bind (`couldn't bind gdb to socket on port 3333`) still holds the bus but answers no port probe — so a port-based check reports "bus released" while a second master is still driving SWCLK. That shared logic lives in `tools/lib-swd.sh`; both scripts source it so they can't drift apart again. `--stop` also can't clear a wedged instance, because openocd only honours telnet `shutdown` when its control port is alive — it now says so instead of reporting success.

**Never** run the `pinctrl set 24 ip pd` SWD continuity probe while an openocd is running. Its bitbang driver sets pin direction once at init and never re-asserts it, so flipping GPIO 24/25 underneath it wedges that instance into `Error connecting DP: cannot read IDR` permanently — a power cycle does not recover it, only restarting openocd does.

Don't build the USB-dev workflow with `rtt.conf` — it turns off CDC-ACM console, which is the whole point of USB iteration.

## Other helpers

- `test-join.sh` — force-rejoin cycle: remove Z2M's cached entry, factory-reset the device, watch it re-interview, PASS/FAIL on the result.

  It removes the Z2M device entry first (step 3) because Z2M keys `interview_state`, `configured_reportings` **and the endpoint's cluster list** by IEEE, and a device-side factory reset invalidates none of it. Without the removal the test can pass off the previous join's cache, and can report a stale cluster list as current — both observed. The cost is that the device's Z2M/HA history resets on every run.

  It prints the advertised input clusters on completion, and `EXPECT_CLUSTERS` asserts them (order-insensitive, exit 4 on mismatch) so a firmware cluster-list change can be regression-tested:

  ```
  EXPECT_CLUSTERS=genBasic,genIdentify,genPollCtrl,genPowerCfg,seMetering ./tools/test-join.sh
  EXPECT_CLUSTERS='10:genBasic,genIdentify,genPollCtrl,genPowerCfg,seMetering' ./tools/test-join.sh
  ```

  The flat form compares the union of input clusters and additionally requires exactly one endpoint — true today (`APP_ENDPOINT` 10, `ZBOSS_DECLARE_DEVICE_CTX_1_EP`). The `ep:clusters` form pins each cluster to its endpoint; use it if the firmware grows a second endpoint, since a cluster *moving* endpoints leaves the union unchanged but breaks Z2M/HA bindings.

  `genPowerCfg` joined the list when battery reporting landed (#8). A cluster-list change also needs the Z2M-side external converter updated in step with it — see `seeed-studio-zigbee-energy-meter/z2m/`.
- `xiao-pulse.sh` / `xiao-pulse-burst.sh` — drive the D7 pulse-simulator GPIO on the Pi to feed synthetic pulses into the meter. Shell + `pinctrl`, so per-edge timing bottoms out at 5–15 ms.
- `xiao-pulse-us.sh` — µs-precision variant for #59's min-pulse-width filter AC test (threshold ± 100 µs boundary discrimination). Writes BCM `GPSET0` / `GPCLR0` directly through `/dev/gpiomem` and busy-waits on `perf_counter_ns`, so sub-millisecond pulse widths are actually accurate. No daemon dependency — user in the `gpio` group runs it without sudo.

## Current measurement

- `ina219-sample.sh` / `ina219-sample.py` — one-shot INA219 CSV over the shunt in the Pi 3V3 → XIAO BAT path. See `docs/working/` for captured baselines.
- `xiao-por.sh` — pulse the BCM 22 relay in the 3V3 rail to force a full POR (clears the CoreSight `CDBGPWRUP` latch that inflates baselines by ~1.5 mA otherwise), then **verify it actually took**.

  The POR is not deterministic. On 2026-07-30 the same script with the same wiring gave 1.512 mA (latch still set) and then −0.097 mA (cleared) on back-to-back runs, and the old version reported success either way — two full captures were burned on a contaminated baseline before anyone noticed. It now settles, takes a short INA219 sample, and classifies: `p50 < 0.5 mA` = clean, a tight `1.0–2.2 mA` plateau = the latch is still set, `> 2.2 mA` = device busy scanning so the latch can't be judged yet. It retries the POR automatically (3 attempts) and exits non-zero rather than handing back a bad baseline.

  ```
  ./tools/xiao-por.sh              # POR + verify
  ./tools/xiao-por.sh 8            # longer cut window if the rail isn't collapsing
  ./tools/xiao-por.sh --no-verify  # pulse only (warns; the check exists for a reason)
  ```

  Exit codes: 0 verified clean, 2 latch still set after every attempt, 3 device never idle enough to judge.

  **Pulse-cost runs need a control.** Holding D7 low sinks +0.242 mA through the pin's ~13 kΩ pull-up. In a 50-pulse burst at 71% low-duty the total delta was +0.204 mA against +0.172 mA predicted from the sink alone — the residual +0.032 mA is about 1 SEM, i.e. the LPCOMP→PPI→TIMER2 path has no measurable CPU wake cost. Subtract a duty-matched control or you'll overstate pulse cost by roughly 6×.
- `measure-power.sh <label> [s] [hz]` — orchestrator for #35: runs `ina219-sample.py` and `z2m-events.py` in parallel over SSH, produces `ina219.csv` + `events.csv` in `docs/working/measurements/<ts>-<label>/`, and renders `plot.png` with current vs. wall-clock annotated by Z2M events. Requires `pip3 install matplotlib` on the Mac. Pi-side scripts stream over SSH — no install on the Pi ahead of time.

## Tooling tests

Host tests for the shell tooling — no Pi, no hardware, no network. `ssh`/`scp`/`sleep` are stubbed and the remote state is faked, so the real decision logic runs against canned inputs in about a second.

```
./tools/tests/test-join-logic.sh ./tools/test-join.sh    # join/interview pass-fail logic
./tools/tests/test-swd-guards.sh ./tools                 # #68 two-openocd bus guards
./tools/tests/test-por-verify.sh ./tools/xiao-por.sh     # POR classification + retry
```

These are regression tests for one specific class of bug: **tooling that reports success while the underlying state is wrong.** A failed Z2M poll counted as proof the device hadn't interviewed yet (#69); a wedged openocd reported as "bus released" (#68); a POR that never cleared the latch reported as done. All three produce a confident green result that then silently poisons a bench measurement — the most expensive kind of failure on this rig, because the number looks plausible.

The POR and SWD suites are seeded with the real measured values (1.512 mA contaminated vs −0.097 mA clean; the 2026-07-30 fail-then-succeed sequence), so they encode what the rig actually does, not what it should do in theory. If you change the logic they cover, run them.

# INA219 baseline + first tuning-pass attempt (issue #8, #35)

Wired up the Adafruit ADA904 INA219 harness per the bench-prep doc,
validated the chip works, then tried to take the "expected today"
baseline before starting on the blocker-#2 (USB VBUS-gate) tuning pass.
The measurements turned up something that has to be resolved before the
blocker ordering makes sense: **blocker #1 (sleepy ED + turbo poll,
shipped speculatively in PR #46) is not delivering sleep behaviour.**

Diagnosis is unfinished — three plausible root causes were ruled out
by measurement, and I ran out of runway before RTT-over-SWD logging
could be set up to see what ZBOSS is actually doing internally.
Details below so the next session can pick this up cleanly.

## INA219 wiring + validation

- I²C bus needs enabling on the Pi (`sudo raspi-config nonint do_i2c 0
  && sudo reboot`). `/dev/i2c-2` on a fresh Pi is the HDMI EDID trap,
  not the header — memory: [[reference_ina219_pi_harness]].
- Chip appeared at `/dev/i2c-1 @ 0x40` after enabling, POR default
  config `0x399F`, bus voltage 3.30 V from Pi 3V3 through the shunt.
- Sampling harness: `tools/ina219-sample.py` (Pi-side, smbus2, big-endian
  register reads) invoked from Mac via `tools/ina219-sample.sh <label>
  [seconds] [hz]`. Writes CSV under this dir with a `# summary` line
  appended after the run.

## Measurement runs

All on the currently-shipping firmware unless noted (`main` at
`9e2e325`, XIAO powered from Pi 3V3 through the INA219 shunt, USB
unplugged). Each row is a 10 Hz sample. `label` matches the CSV file
name for cross-reference.

| label | duration | mean_mA | sd | min | max | notes |
| --- | ---:| ---:| ---:| ---:| ---:| --- |
| `baseline-blocker1-only` | 60 s | 6.71 | 0.27 | 6.2 | 9.6 | Device not joined (Pi-3V3 boot didn't reattach) |
| `long-diagnose` | 180 s | 7.09 | 0.16 | 6.6 | 7.6 | Still not joined; radio in scan mode |
| `post-fresh-join` | 180 s | 15.04 | 2.97 | 1.8 | 23.8 | Immediately after test-join.sh — Z2M is still exchanging |
| `settled-postjoin` | 180 s | 12.43 | 0.17 | 10.9 | 13.7 | 5 min after fresh join — flat at 12.4 mA |
| `no-log-diag` | 240 s | 13.51 | 4.56 | 1.2 | 27.7 | CONFIG_LOG=n |
| `no-turbo-poll` | 240 s | 13.61 | 4.60 | 1.3 | 31.5 | Turbo poll call removed |
| `usb-off` | 240 s | 13.52 | 4.53 | 1.2 | 18.7 | `usb_disable()` right after `usb_enable()` |

## What we know

- **The stack CAN sleep**: in the first ~15 s of the fresh-join runs,
  before Z2M starts its ConfigureReporting exchange, we see means as
  low as 2.2 mA with 150+ samples down at 1.5 mA. Sleep works.
- **Baseline steady-state is 12.4 mA continuous, dead flat** (sd ≤ 0.2
  once Z2M has quiesced). That's exactly `CPU active + radio in RX`
  per the nRF52840 datasheet (~5 mA CPU + ~5 mA RX + ~2 mA LDO).
- **Joined-idle draws MORE than joined-scanning** (12 mA vs 7 mA). A
  sleepy ED should be much lower joined-idle than scanning — this
  reversal is the strong signal that `rx_on_when_idle` is stuck TRUE
  after join even though we call `zb_set_rx_on_when_idle(FALSE)` at
  boot per the NCS docs.

## Ruled out by direct measurement

Each was flashed via `FLASH_METHOD=swd tools/test-join.sh` and sampled
for 4 minutes on a confirmed-fresh join.

1. **Chatty logging pinning the CPU** (`CONFIG_LOG_MODE_IMMEDIATE=y` +
   `CONFIG_ZIGBEE_APP_UTILS_LOG_LEVEL_DBG=y`) — tested with `CONFIG_LOG=n`.
   Same 12 mA steady state.
2. **`zb_zdo_pim_start_turbo_poll_continuous` clobbering rx-on-when-idle**
   — removed the call entirely; only `zb_zdo_pim_set_long_poll_interval`
   remained. Same 12 mA steady state.
3. **USB device stack keeping HFCLK on** — `usb_disable()` immediately
   after the SYS_INIT-time `usb_enable()`. Same 12 mA steady state.

## Still on the table

- `CONFIG_PM=y` + `CONFIG_PM_DEVICE=y` — Zephyr's device power
  management. Bench-prep doc warns against enabling these without
  measurements (NCS 2.9.2 users have reported USB-stack interactions);
  we now HAVE measurements saying we need SOMETHING.
- `CONFIG_ZIGBEE_TIME_KTIMER=y` — the low-power-snippet workaround for
  the NCS 2.9.x + TIMER2-in-overlay 10 µA leak. We use TIMER2 for
  pulse counting, so we're inside the affected window. Won't fix 12 mA
  on its own, but should be set anyway for correctness.
- **ZBOSS internally clobbering rx-on-when-idle after join** — cannot
  be inspected without RTT-over-SWD logging (USB is unplugged for the
  measurement rig). Add `CONFIG_USE_SEGGER_RTT=y` +
  `CONFIG_LOG_BACKEND_RTT=y` and drive `openocd rtt server` on the Pi
  to get logs during battery-mode runs. That's the tool that unblocks
  further diagnosis.
- **The XIAO Sense variant may have onboard peripherals (IMU, MEMS
  mic) that boot enabled** — verify with the XIAO nRF52840 Sense
  schematic and check whether any need explicit suspension. Likely
  small (<1 mA), but should be excluded.

## Products of the session

Landing on the tree (uncommitted, ready to commit):

- `tools/test-join.sh` — new `FLASH_METHOD` env var (`auto|usb|swd`),
  SWD added as auto-mode fallback after flash.sh/flash-serial.sh.
  Makes off-USB iteration on the INA219 rig tractable.
- `tools/ina219-sample.py` + `tools/ina219-sample.sh` — CSV sampling
  harness for the INA219 on rpi-xiao. Streams `timestamp_ms,bus_mV,
  shunt_uV,current_mA,ready,overflow` at configurable rate/duration
  with a metadata header (start time, config reg, firmware commit).
- `diagnose-no-log.conf` — one-line overlay (`CONFIG_LOG=n`) used to
  isolate the logging-chatter hypothesis. Kept as reference for
  future diagnostic runs; not for production builds.
- 8 CSVs in this dir, matching the table above. Raw data for anyone
  cross-checking the analysis or continuing the diagnosis.

Reverted at end of session (both were diagnostic hypothesis tests, both
disproven, both would confuse a reader if left in the tree):

- The turbo-poll removal in `src/zigbee_app.c`.
- The `usb_disable()` addition in `src/main.c`.

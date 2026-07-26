# Battery-mode transition and sleep-current measurement (issue #8)

Bench-prep doc for the physical + measurement work. Written before any
measurements exist, so it captures the plan and a code audit of known
sleep-blockers rather than firmware changes — those come after the PPK2
numbers say what actually matters.

## Physical rewiring checklist

Sequence matters — do the sensor rewire *before* moving power to VDD so
you never source-select-fault the phototransistor.

1. **Move phototransistor collector**: unsolder from 3V3 pin, re-solder
   to VDD (or the direct BAT/VDD tap per the design doc). At battery
   voltage (~3.0 V from 2×AAA lithium) the collector resistor divider
   still lands the LPCOMP swing across the VDD × 3/8 threshold we set in
   `hw_pulse_counter.c` — the threshold is a ratio of VDD, so it tracks
   supply voltage automatically. No firmware change to LPCOMP.
2. **Bench verify sensor still triggers**: with the XIAO still on USB,
   run `tools/test-join.sh`, then shine a torch across the snout.
   Confirm serial log shows `pulse(s) counted: delta=…` and Z2M's
   `zigbee2mqtt/0xf4ce361b0656e80e` topic reflects an incrementing
   `energy` field.
3. **Mount 2×AAA holder** in the printed compartment using the Bambu
   kit's spring terminals. Leads to BAT/VDD pads on the XIAO. Verify
   polarity twice — the XIAO has no reverse-polarity protection on BAT.
4. **Insert cells** (Energizer Ultimate Lithium AAA), unplug USB, watch
   the boot blink. Four blinks = `main()` reached under battery. If you
   get anything else (1 blink loop = CDC-ACM device_not_ready, 500ms
   loop = button configure fail), stop and read the serial log via
   `/dev/cu.usbmodem*` on a USB replug.

## PPK2 measurement plan

Hook the PPK2 in source-meter mode: PPK VOUT → BAT pad, PPK GND → XIAO
GND, cells REMOVED from the holder so the PPK is the only supply. Set
PPK VOUT to 3.0 V (matches 2×AAA fresh) and enable the current
sampling.

Measurements to record (each as a CSV export + one screenshot of the
scope trace):

| State | Trigger | Expected today | Target after tuning |
| --- | --- | --- | --- |
| Deep-sleep between wakes | Idle, no pulses, joined | **~5 mA (rx-on)** | 2–3 µA (System-ON + LPCOMP retained) |
| LPCOMP pulse-count event | Torch flash, no report | Same as above (no wake) | Same |
| Zigbee poll / keepalive | Sleepy ED parent poll | N/A today (rx-on-when-idle) | ~10 mA burst, ~3 ms duration |
| Report frame TX | 5-min tick or heartbeat | ~15 mA burst, ~5 ms | Same |
| Rejoin after parent loss | Force network drop | ~10 mA sustained ~5 s | Same (unavoidable) |
| Boot from reset | Power cycle | ~10 mA for ~2 s | Same (unavoidable) |

Battery-life projection formula (from the issue's acceptance criteria):

```
avg_current_uA = sleep_uA
               + poll_uA_burst   × (poll_burst_s / poll_interval_s)   × 1e6
               + report_uA_burst × (report_burst_s / report_interval_s) × 1e6
life_hours = 1200 mAh × 1000 / avg_current_uA
life_years = life_hours / 24 / 365
```

Use Energizer Ultimate Lithium AAA's ~1200 mAh capacity at low-drain
discharge (their datasheet's 25 mA constant-drain figure — we're way
below that so it's the right end of the curve).

## Interim measurement path (INA219 on Pi, pre-PPK2)

For blockers #1–#4 below the average current is still in the mA range,
which is well within an INA219 breakout's ~10 µA resolution floor.
Cheaper and lets the tuning loop run off the Pi rig we already have
(SWD flash from #34 + I²C measurement here) without waiting on PPK2
availability. Tracked in #35.

Board: [Adafruit ADA904 via Core Electronics](https://core-electronics.com.au/ina219-high-side-dc-current-sensor-breakout-26v-3-2a-max.html)
(~$17 AUD, STEMMA QT + screw terminals pre-soldered; 0.1" header loose).
Optional [Qwiic-to-jumper cable](https://core-electronics.com.au/qwiic-cable-breadboard-jumper-4-pin.html)
avoids soldering the header entirely.

The INA219 sits in two circuits at once. The XIAO does NOT wire to the
INA219's logic side — only its power line passes through the on-board
shunt.

**Circuit 1 — Pi ↔ INA219 (logic bus, via header pins OR STEMMA QT)**

| INA219 | Pi header pin | Function |
| --- | --- | --- |
| VCC | Pin 1 (3V3) | INA219 chip logic power |
| GND | Pin 6 (GND) | Shared ground (already there for SWD harness) |
| SDA | Pin 3 (GPIO 2 / I²C1 SDA) | I²C data |
| SCL | Pin 5 (GPIO 3 / I²C1 SCL) | I²C clock |

Verify with `i2cdetect -y 1` on the Pi (default addr `0x40`) after
enabling I²C via `raspi-config`.

**Circuit 2 — Pi 3V3 → XIAO BAT via INA219 shunt (power path, screw terminals)**

```
Pi pin 1 (3V3) ──22 AWG──> INA219 Vin+ (screw terminal)
                                │
                          [0.1 Ω shunt]
                                │
                           INA219 Vin- (screw terminal) ──22 AWG──> XIAO BAT pad
```

Use the screw terminals here (not the header's Vin+/Vin- pins) — same
nets electrically, but the terminals clamp thicker wire and let you
swap the load-side wire (Pi 3V3 → bench PSU → actual AAA cells)
without soldering each time.

Do NOT plug USB into the XIAO while this shunt is powered from the Pi
3V3 rail. Two supplies in parallel with different regulators — remove
one before adding the other.

When post-tuning sleep current drops below ~50 µA, INA219 results become
unreliable and we switch to PPK2 for the final numbers that close #8.

## Known sleep-blockers in today's firmware

Read this alongside the CSVs — they explain the "expected today"
column above. Every one of these will need a fix pass before we get
close to the target column, so the baseline measurement is really a
sanity check that the physical rewire didn't break anything and that
the tuning delta between "before" and "after" is measurable.

Ordered by expected impact on average current:

1. **`zigbee_configure_sleepy_behavior(false)`** at
   `src/zigbee_app.c:316`. Explicitly turned off for USB-dev.
   Comment (lines 294–314) documents the reason: Z2M's ZDO interview
   times out on a sleepy ED because the default parent-poll cadence
   (~7.5 s) is slower than Z2M's read deadline. Fix: flip to `true`
   AND tighten the initial poll interval for the join window, then
   loosen it after interview completes. This alone should take us
   from ~5 mA to <100 µA average.

   **SPECULATIVE LAND (no measurements yet)** — implemented on this
   branch. Three Kconfig knobs, all under a new `Zigbee sleepy-ED
   behaviour (issue #8 blocker 1)` menu:
   - `APP_ZIGBEE_SLEEPY_ED=y` (default) — flips
     `zb_set_rx_on_when_idle` to FALSE at boot
   - `APP_ZIGBEE_JOIN_TURBO_POLL_MS=30000` — on
     `ZB_BDB_SIGNAL_STEERING`+RET_OK the signal handler calls
     `zb_zdo_pim_start_turbo_poll_continuous(30_000)` to cover Z2M's
     interview reads at ~100 ms poll cadence; ZBOSS auto-reverts to
     the long-poll interval when the window expires (no separate
     leave callback needed)
   - `APP_ZIGBEE_LONG_POLL_INTERVAL_MS=60000` — set via
     `zb_zdo_pim_set_long_poll_interval` right before the turbo call
     (ZBOSS API note: long-poll config only valid AFTER join, during
     steering it snaps back to the default 5 s)

   Untested assumptions to confirm at the bench:
   - Does the 30 s turbo window actually cover a fresh Z2M interview,
     including retries after a bad first parent choice?
   - Does the auto-rejoin path (`ZB_ZDO_SIGNAL_LEAVE` → new
     `ZB_BDB_SIGNAL_STEERING`) re-apply the poll intervals cleanly?
     (Same signal handler block runs on any RET_OK steering.)
   - Any regression in `tools/test-join.sh` under sleepy — Z2M's
     interview could still fail if the coordinator is slow.

   If the interview breaks, `dev.conf` can flip
   `APP_ZIGBEE_SLEEPY_ED=n` for a temporary rollback while
   measurements pinpoint the failing knob.

   **Decision to revisit once INA219 (#35) is on the bench**: turbo
   poll vs. a plain short long-poll (light_switch-sample style, e.g.
   3-5 s always). Turbo poll assumes the 60 s long-poll savings
   dominate the 30 s / rejoin burst cost; the INA219 CSV is what
   proves or refutes that. If a flat 3 s long-poll hits the sleep-
   current target inside a factor of ~2, drop the turbo dance and
   simplify to one `zb_zdo_pim_set_long_poll_interval()` in init.
   If not, keep turbo and tune the two window/long-poll values.
2. **USB CDC-ACM permanently enabled** — `CONFIG_USB_DEVICE_STACK=y`,
   `CONFIG_USB_CDC_ACM=y` in `prj.conf`, unconditional `usb_enable()`
   in `main.c:295`. USB draws mA even when idle. Fix: detect VBUS at
   boot and only enable USB when present; otherwise leave the CDC-ACM
   driver out or held in suspend. Overlay change: make USB conditional
   on VBUS-sense GPIO.

   **SPECULATIVE LAND (no measurements yet)** — implemented on the
   `zigbee-usb-vbus-gate` branch on top of blocker 1. New Kconfig
   under "USB CDC-ACM VBUS gate (issue #8 blocker 2)":
   - `APP_USB_VBUS_GATE=y` (default) — read
     `nrf_power_usbregstatus_vbusdet_get(NRF_POWER)` at boot; VBUS
     absent skips `usb_enable()` + the 5 s DTR wait (folds in
     blocker 7 below since the fix is identical). VBUS present
     keeps the USB-dev flow.
   - `prj.conf` sets `CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=n` to
     stop Zephyr's SYS_INIT hook from calling `usb_enable()` before
     we check VBUS. Overrides the `xiao_ble` board default of `y`.
   - The CDC-ACM device_not_ready fatal-LED path only fires when we
     WANT USB (VBUS present). On battery the CDC device object
     stays idle and unclaimed.

   Bench-verified end-to-end for the VBUS-absent path (XIAO powered
   from Pi 3V3 via BAT pad, USB physically unplugged): fresh join
   NOT_JOINED → IN_PROGRESS → SUCCESSFUL in ~24 s, pulse-to-Z2M
   still works (100-pulse burst produced a metering report at
   `energy=2.77 kWh`).

   Not yet verified:
   - VBUS-present path — needs someone at the bench to plug USB and
     confirm CDC-ACM still enumerates and the DTR wait still fires
     under Sequoia. `APP_USB_VBUS_GATE=n` in `dev.conf` is the
     escape hatch if the VBUS reading is wrong.
   - Current-draw delta (INA219 #35 required).
3. **200 ms `k_sleep` polling loop** at `main.c:497` inside `while(1)`.
   Zephyr's tickless idle plus `k_sleep` DO enter System-ON sleep
   between wakes on nRF52, so this isn't as bad as it looks — the CPU
   is genuinely off for most of the 200 ms — but the wake itself has a
   ~1 ms startup cost and burns average current at 5 Hz for nothing.
   The hardware counter is retained during System-ON; there's no
   physical reason to read it more than once per report interval. Fix:
   replace the polling loop with a delayable work item at 5-min
   cadence that reads the counter, publishes the delta, and reschedules.
4. **LED heartbeat every 5 wakes** — `HEARTBEAT_TOGGLE_EVERY` at
   `main.c:80`. On battery the LED is a lit-during-half-cycle current
   source (~1–2 mA at 3.0 V through the on-board resistor). Fix: gate
   the heartbeat on USB-attached, or drop it entirely and rely on the
   serial log / Z2M state as liveness signal.
5. **`CONFIG_LOG_MODE_IMMEDIATE=y`** in `prj.conf:23`. Immediate-mode
   logging blocks the caller while it writes to the backend, which
   costs CPU time (== current) at every LOG_INF call. Fix: switch to
   deferred mode (`CONFIG_LOG_MODE_DEFERRED=y`) and drop the log
   level to WRN for the field build; keep DBG behind a Kconfig for
   dev builds.
6. **`CONFIG_ZIGBEE_APP_UTILS_LOG_LEVEL_DBG=y`** in `prj.conf:90`.
   Chatty during join. Fine on USB, expensive on battery even with
   deferred logging. Fix: drop to INF or WRN for field builds.
7. **`wait_for_host_dtr_or_timeout(cdc, 5000)`** at `main.c:393`.
   Adds 5 s of boot-time CPU-on wake before Zigbee even starts. Not
   a steady-state issue but a first-boot / factory-reset cycle cost.
   Fix: gate on VBUS-present at boot, skip if on battery.

   **FOLDED INTO BLOCKER 2** — the fix is literally the same VBUS
   check, and the DTR wait can only usefully fire when USB is
   attached, so gating them together made more sense than a
   separate PR.
8. **`CONFIG_PM=y` not set**. Zephyr's PM subsystem manages device
   power states and CPU idle states. On nRF52 the default `arch_idle`
   still enters System-ON, so this is a smaller lever than the ones
   above, but combined with `CONFIG_PM_DEVICE=y` it enables per-driver
   suspend that stops each driver keeping its clock/regulator on
   between uses. Fix: enable both, then verify with the PPK trace that
   the driver stack actually goes quiet between wakes.

Not blockers but worth noting:

- **Basic cluster `power_source` is already `BATTERY`** at
  `zigbee_app.c:247`. So we advertise as battery-powered even in
  USB-dev, which is correct for the target model.
- **`CONFIG_ZIGBEE_ROLE_END_DEVICE=y`** in `prj.conf:75` — MAC-layer
  ED role is set. Only the runtime rx-on-when-idle flag needs to
  change; no compile-time reconfiguration.
- **NRF_802154 uses TIMER0, TIMER1, RTC2** — plenty of unclaimed
  peripherals for future work. TIMER2 is ours for pulse counting.

## Ordering of the firmware tuning pass

Do them in the "known blockers" order above and re-measure between each,
so the CSV shows a stair-step improvement and we can point at any
regression clearly. Blockers 1 and 2 will dominate the improvement;
blockers 5–8 are polishing.

Any tweak beyond blockers 1–4 should be justified against a CSV
showing the change actually moves the number. Don't preemptively
enable PM configs for their own sake — several NCS 2.9.2 users have
reported PM_DEVICE interacting badly with the USB stack, and we don't
want to fight that battle without a measurement to justify it.

## What to commit back to the repo

Per the issue's acceptance criteria:

- `docs/working/2026-07-<date>-ppk-baseline.md` with the baseline
  numbers, screenshots, and battery-life projection at baseline
- Firmware tweak PRs, one per blocker, each linking back to a CSV
  showing the delta it produced
- Final `docs/working/2026-07-<date>-ppk-tuned.md` with the tuned
  numbers and updated projection
- Close #8 when tuned average current × 1200 mAh > 1 year projected
  life (design-doc target is multi-year, so 1 year is the "we can
  stop tuning" bar)

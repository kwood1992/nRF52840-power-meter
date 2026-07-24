# 2026-07-24 — Explicit Report-Attributes fallback for silent-Z2M bug (#20)

## Symptom being addressed

From issue #20: after multiple factory-reset cycles the same joined+
interviewed device stops publishing to `zigbee2mqtt/<ieee>` even though
pulses land in the accumulator, `ZB_ZCL_SET_ATTRIBUTE` runs on the
ZBOSS thread, and `configured_reportings` on the coordinator still
lists `currentSummDelivered` (delta=100). Delta-triggered reports go
silent; only max_interval (65000 s ≈ 18 h) would eventually force one.

## What changed

**`src/report_gate.{h,c}` (new, pure logic, host-tested)** — every-Nth-
call counter. `report_gate_advance()` returns true exactly every
`period` calls; `period == 0` disables it. Trivial API, but split out
so the "should this pulse also force an explicit report?" decision
lives in a testable module rather than as an ad-hoc counter inside
`main.c`. Test cases cover the disabled path, `period == 1`, the exact
`Nth` boundary, `period == 100` (production value), and the off-by-one
"does the counter reset properly between fires" case.

**`src/zigbee_app.{h,c}` — new `zigbee_app_publish_summation_and_report()`.**
Same shape as the existing `_publish_summation`, but the ZBOSS-thread
trampoline additionally calls `zb_zcl_find_reporting_info` for our EP/
cluster/attribute, allocates an OUT buffer, and invokes
`zb_zcl_send_report_attr_command` — explicitly emitting a Report
Attributes frame bypassing the reporting engine's delta calculation.
Two graceful-degradation branches:

- `find_reporting_info` returns NULL → log `WRN "no reporting slot …
  coordinator ConfigureReporting not seen yet"` and skip. The attribute
  write still happened; the reporting engine will retry the next time
  Z2M configures reporting (or on the next `_and_report` call).
- `zb_buf_get_out()` returns 0 → log `WRN "no free OUT buffer"` and
  skip. Buffer pool exhaustion is transient; next tick recovers.

The plain `_publish_summation` path is unchanged so per-pulse writes
stay cheap.

**`src/main.c` — two callsites switch to `_and_report`.**
- The 5-minute `metering_report_work_handler` tick now always forces
  an explicit report. This closes the design-doc gap where "one report
  per 5 min" was actually "one report every N pulses OR every 18 h,
  whichever comes first."
- The per-pulse publish path in the sample loop uses
  `report_gate_advance()` with `ZIGBEE_REPORT_PULSE_HEARTBEAT = 100`.
  On the 100th pulse-driven publish since boot (or since the last
  fire), the force-report variant runs. At the typical bench 1 Hz
  injection rate that's a heartbeat every ~100 s — bench-visible
  quickly enough that a silent-reporting failure surfaces in a
  single test cycle instead of at the 5-min tick.

**Boot-time initial publish stays on plain `_publish_summation`.**
That call fires before ZBOSS has joined + been interviewed, so a
`WRN "no reporting slot"` there would be noise on every boot.

## Why not just re-install the reporting slot ourselves?

Considered `zb_zcl_put_reporting_info` with defaults matching what
Z2M's `configured_reportings` shows (delta=100, min=0, max=65000).
Rejected because:

1. The destination fields (short_addr, endpoint, profile_id) aren't
   trivially discoverable device-side — a Sleepy ED doesn't hold a
   fresh short-address for the coordinator without going through
   NWK/APS discovery. Getting the destination wrong sends reports
   to a black hole; getting it right requires more code than the
   explicit-send path.
2. The design-doc contract with Z2M is "coordinator configures
   reporting, device honors it." Device-side re-configuring conflicts
   with that contract — if Z2M later updates the reporting parameters
   (change delta from 100 to 10 during commissioning experiments),
   we'd have to know to not clobber it.

Explicit-send-when-a-slot-exists preserves the contract: we don't
create slots, we just don't rely on the reporting engine's delta
gate to know when to send.

## Cost budget

Bandwidth: one extra ZCL Report frame per 5 min plus one per 100
pulses. At 100 imp/kWh that's 1 report per 100 kWh, or once per
1000 kWh at a 1000-imp/kWh meter (i.e. essentially never for a
normal household). Well under 0.01 % duty cycle change.

Radio wake in future sleepy build: the 5-min tick already wakes the
radio for the reporting-engine's own delta-check + potential frame,
so forcing an explicit frame in the same wake window is amortised.
No extra wake required.

RAM/flash: the counter is 8 bytes on the stack; the explicit-send
path adds ~40 bytes to `libapp.a`. Build memory report unchanged at
the megabyte scale (47.14 % flash, 28.56 % RAM).

## Bench-verified 2026-07-24

Confirmed against the real Z2M + ZStack3x0 coordinator through the
Pi/SWD rig. Same coordinator, same device (`0xf4ce361b0656e80e`),
same channel 18 as prior sessions.

Sequence and outcome:

1. `tools/flash.sh` failed — the XIAO's Adafruit UF2 bootloader stopped
   advertising its mass-storage USB endpoint (CDC-ACM enumerates on
   `/dev/cu.usbmodem*`, but no `/Volumes/XIAO-SENSE` mount and no
   external `/dev/disk*`; `system_profiler SPUSBDataType` shows the
   bootloader with no Interfaces subsection). Persisted through USB
   replug on this session. Not a firmware issue — the app-mode
   descriptor still lists our custom VID 0x1915 / PID 0x520f and
   Product string "XIAO Zigbee Energy Meter" fine.

2. Worked around it via `adafruit-nrfutil dfu serial` over the
   bootloader's CDC-ACM endpoint:

    pip3 install --user adafruit-nrfutil
    adafruit-nrfutil dfu genpkg --dev-type 0x0052 \
        --application build/zephyr/zephyr.hex --sd-req 0xFFFE \
        <scratch>/app-dfu.zip
    adafruit-nrfutil dfu serial \
        --package <scratch>/app-dfu.zip \
        --port /dev/cu.usbmodem<n> -b 115200 --singlebank

    That completed in ~15 s with `Device programmed.` — the app came
    back up on VID 0x1915 / PID 0x520f as expected. Kept as fallback:
    if the mass-storage endpoint stays flaky, a small `tools/flash-serial.sh`
    that wraps the two `adafruit-nrfutil` calls would land the same
    UF2 payload every time.

3. `~/z2m-cli pub bridge/request/device/remove '{"id":"0xf4ce...","force":true}'`
   — Z2M held a stale `interview_state: SUCCESSFUL` cache entry for
   the device even though it had never actually re-associated after
   the last session's factory-resets (`last_seen: null`,
   `configured_reportings: null`). This IS the coordinator-side stale
   binding predicted by #20's suspected cause (1). Force-removing the
   entry unblocked the fresh join. **Test-harness gap surfaced by
   this: `tools/test-join.sh` polls only `interview_state`, which
   satisfies against Z2M's stale cache and returns PASS on a device
   that has actually never rejoined. Follow-up: cross-check
   `configured_reportings != null` or a fresh `last_seen` timestamp
   before returning SUCCESS.**

4. `permit-join true 254` + short-press D6 to steer — device joined
   and Z2M ran the interview: `SUCCESSFUL` on ~poll #7 (~24 s).

5. `~/xiao-pulse-burst.sh 105` fires 105 falling edges on D7. The
   sample loop advances the accumulator; the per-pulse publish path
   calls `_and_report` on the 100th pulse; the report frame goes out
   directly regardless of the reporting-engine's delta state.

6. `zigbee2mqtt/0xf4ce361b0656e80e` published
   `{"energy":0.21,"linkquality":162,"power":0}` within the 40 s
   collection window. `energy: 0.21` = 210 pulses (5 restored +
   205 fired across this session's burst work) ÷ 1000 divisor =
   0.21 kWh — matches on-device serial log
   `persisted accumulator_total=210`. Z2M's external converter is
   decoding correctly.

7. Even though `configured_reportings: 0` in Z2M's snapshot at the
   time of the burst, the report frame still landed and Z2M still
   processed it. That's the explicit-send path bypassing the
   reporting-engine's dependence on Z2M's ConfigureReporting
   bookkeeping being in sync — the primary win of this fix.

## Follow-ups noted for future issues

- **`tools/test-join.sh` false-PASS on stale Z2M cache** (see step 3
  above). Small — worth a one-line fix that adds
  `configured_reportings != null` to the SUCCESS gate, plus a doc
  note on running `bridge/request/device/remove force:true` before
  test-join.sh cycles to ensure the coordinator side is clean.
- **XIAO bootloader mass-storage endpoint sometimes fails to
  advertise** — cost-of-workaround is `adafruit-nrfutil` install
  + serial-DFU path. If this recurs, adding
  `tools/flash-serial.sh` for the fallback is ~20 lines of shell.
- **Verify the fix survives ≥5 sequential factory-resets** — this
  session did one full clean cycle end-to-end, which is enough to
  confirm the explicit-send path works, but the original silent-Z2M
  report was after "many factory-reset + rejoin cycles via
  `tools/test-join.sh`". Doing the 5-cycle regression once
  test-join.sh is hardened would close the "always works" question
  vs. the "works on the first fresh join" question.

## Related

- #20 (the issue)
- #17 / #5 (original Metering cluster + ConfigureReporting install)
- #7 (LPCOMP hardware chain) — retires the pulse-driven publish path
  but keeps the 5-min tick, so the `_and_report` change on the tick
  survives that refactor

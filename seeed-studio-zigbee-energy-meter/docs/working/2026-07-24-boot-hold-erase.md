# 2026-07-24 — Boot-hold accumulator erase (#14)

## Why this exists

The XIAO's `CurrentSummationDelivered` is a lifetime-of-device counter
persisted in NVS across reboots. Moving the sensor to a different
physical meter — or reusing an ex-dev-bench board on a new home install
— means the accumulated total no longer reflects reality. Users need a
first-party way to zero it that:

- Works identically for ZHA and Zigbee2MQTT users (no per-integration
  converter code).
- Can't be triggered by accident during normal use.
- Doesn't collide with the existing sw0 short-press (join) / long-press
  (factory reset) bands.

A boot-time button-hold gesture satisfies all three — it's the same
recovery-pin gesture printers, routers, and industrial controllers have
used for decades, and it's essentially impossible to trigger accidentally
because it requires the button to be held **before power is applied.**

## The gesture

**Hold sw0 (XIAO D6 = P1.11) continuously from power-on / reset for
~3 seconds.** On successful erase:

- The NVS accumulator record is overwritten with 0.
- The red LED (P0.26) blinks 4 times, 150 ms on / 150 ms off — visually
  distinct from the 800 ms boot indicator (4× 100 ms) and the 1 Hz
  heartbeat.
- `LOG_INF("accumulator erased by boot-hold")` fires (may be missed by a
  serial monitor that attaches post-boot; the LED is the primary
  confirmation).

If released before 3 seconds elapse, no erase happens and boot continues
normally — the persisted total is loaded and the sample loop proceeds.

## Timing

Total hold time from power-on ≈ 1 s of boot warm-up (Zephyr init +
`blink(4, 100, 100)` boot indicator + GPIO/ADC configure + NVS mount) +
3 s of continuous hold ≈ **4 seconds**. The boot indicator's dark
interval between blinks is short enough that "start pressing before
plugging in the USB" naturally covers the whole window.

## Ordering constraints

`boot_button_held()` must run:

- **after** `nvs_store_init()` — otherwise `nvs_store_save_total(0)`
  has no backing store.
- **before** `nvs_store_load_total()` — so the load reads back the just-
  written 0 and the sample loop's restore path treats the erased state
  as a normal `total=0` boot rather than needing a bespoke reset flow.
- **before** `user_button_arm_irq()` — with the edge IRQ live, releasing
  the held button would fire `user_button_isr`, queue a release event,
  and the classifier would misread the multi-second hold as a long-press
  factory reset. Splitting the old `user_button_setup()` into
  `user_button_configure()` (pin as INPUT, safe to poll) and
  `user_button_arm_irq()` (edge interrupt live) keeps the two phases
  separable.

Boot sequence, post-#14:

1. LED configure + boot blink
2. USB / ADC / `user_button_configure` (INPUT only) / `pulse_input_setup`
3. `pulse_accumulator_init` / `pulse_edge_detector_init` / `persist_policy_init`
4. `nvs_store_init`
5. **`boot_button_held(3000)` → if true: `nvs_store_save_total(0)` + LED confirm**
6. `nvs_store_load_total` → `pulse_accumulator_restore`
7. `wait_for_host_dtr_or_timeout(cdc, 5000)`
8. `LOG_INF`s / `button_press_classifier_init` / `zigbee_app_init`
9. `user_button_arm_irq` — from here on button presses go through the
   classifier / join / factory-reset paths
10. Sample loop

## Alternatives rejected (from the #10 grill)

| Alternative | Why not |
|---|---|
| Runtime very-long press (>20 s) | Footgun during pairing / bench iteration |
| Z2M custom cluster command | Leaves ZHA users out |
| Manufacturer cluster + converters for both ZHA and Z2M | Doubles converter maintenance for a one-shot action |
| Standard ZCL "reset accumulator" command | Metering cluster 0x0702 has no such command HA integrations expose out-of-the-box |

## Bench verification

**Erase path.** With a non-zero persisted total (drive some pulses via
`ssh rpi-xiao '~/xiao-pulse-burst.sh N'` and wait for a persist tick):

1. `ssh rpi-xiao 'gpio -g mode 17 out; gpio -g write 17 0'` — pull D6
   LOW and hold it (Pi drives active-low).
2. Reflash / cycle power via `tools/flash.sh` (double-tap reset).
3. Watch serial: expect `accumulator erased by boot-hold`, then
   `restored accumulator_total=0 from NVS`.
4. Release: `ssh rpi-xiao 'gpio -g write 17 1; gpio -g mode 17 in'`.
5. Fire fresh pulses; confirm the accumulator counts from 0.

**No-regression path.** Standard bench cycle without holding sw0:
`tools/test-join.sh` should still exit `PASS`, and the serial log should
show `restored accumulator_total=N from NVS` at boot (or "cold boot" on
a truly fresh device) exactly as before.

## Follow-ups deferred

- README install / redeploy blurb referencing this gesture — the
  Zigbee-side docs already tell users how to re-pair the device after a
  factory reset; the "moved to a new meter" flow can grow a short "hold
  the button while plugging in USB to reset the total" bullet once we
  have a battery-mode bench (#8) to validate the full field flow.
- Extending the gesture to also wipe ZBOSS pairing state, so
  "boot-hold" becomes a single-gesture full factory reset instead of
  two separate flows. Not urgent — the existing runtime long-press
  covers the Zigbee side, and combining them risks accidental
  re-pairing on a boot-hold that was only meant to zero the meter.

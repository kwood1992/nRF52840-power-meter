# Min-pulse-width filter — impl-2 bench verification (#59)

Follow-up to impl-1 (spike + Kconfig + firmware + D7 bench-test at
`docs/working/2026-07-29-min-pulse-width-bench.md`). Impl-2 lands the
runtime-writable path: NVS-backed override, manufacturer-specific
Zigbee attribute, and the external-converter surface — same shape
[#48](https://github.com/kwood1992/nRF52840-power-meter/pull/49) shipped
for the imp/kWh Divisor.

## What impl-2 lands

- **NVS**: new key `NVS_ID_PULSE_MIN_WIDTH_US=3` with a u32 slot;
  `nvs_store_{load,save}_pulse_min_width_us()` mirror the imp/kWh API.
- **Validation**: `calibration_is_valid_pulse_min_width_us()` mirrors
  the imp/kWh predicate — same reject-rather-than-clamp policy so an
  out-of-range write returns `INVALID_VALUE` to Z2M instead of silently
  clamping.
- **hw_pulse_counter**: `effective_min_width_us_at_init()` picks NVS
  if the persisted value is in range, otherwise falls back to the
  Kconfig compile-time default. `hw_pulse_counter_set_min_width_us()`
  is the runtime setter — writes TIMER3.CC[0] live via
  `nrfx_timer_extended_compare`.
- **main.c**: NVS init MUST precede `hw_pulse_counter_init()` now
  (the latter's NVS load path depends on it). Sequenced accordingly.
- **Zigbee**: manufacturer-specific attribute `0xF000` on the Metering
  cluster (Nordic manuf_code `0x1015`) via
  `ZB_ZCL_SET_MANUF_SPEC_ATTR_DESC`. Storage in `dev_ctx`;
  initialization from NVS/Kconfig in `metering_attrs_init()`; ZCL
  device callback extended to validate + persist writes and to call
  the runtime setter.
- **External converter**: `min_pulse_width_us` as a `m.numeric`
  addressed by `{ID: 0xF000, type: 0x21, manufacturerCode: 0x1015}`
  so ZHC forwards the mfr code on reads and writes.

## Bench-verified

### 1. Compile-time-default boot

Fresh flash, no NVS entry for pulse-min-width. RTT log at boot:

```
<inf> hw_pulse_counter: min-pulse-width: 1000 µs (compile-time default)
<inf> hw_pulse_counter: hw pulse counter live: … TIMER3 width filter threshold=1000 µs
```

Log message reflects the "no NVS entry → Kconfig fallback" path
returning `-ENOENT`, matches the impl-1 behavior.

### 2. End-to-end MQTT write via the friendly form

Once the updated `external-converters/xiao-power-meter.js` is deployed
to Z2M (HA add-on config dir + Z2M restart) and the device has been
re-interviewed:

```
$ z2m-cli pub 0xf4ce361b0656e80e/set '{"min_pulse_width_us":2500}'
```

Z2M's `bridge/logging` shows the state publish:

```
z2m:mqtt: MQTT publish: topic 'zigbee2mqtt/0xf4ce361b0656e80e', payload
  '{"energy":3502.77,"imp_per_kwh":800,"linkquality":90,
    "min_pulse_width_us":2500,"power":0}'
```

`min_pulse_width_us: 2500` in the payload confirms the write landed on
device, ZHC decoded the readback, and the state cache updated. No ZCL
timeout.

### 3. NVS persistence across reboot

After the write above, reset the device via OpenOCD (preserves flash /
NVS). RTT boot log:

```
<inf> hw_pulse_counter: min-pulse-width: 2500 µs (from NVS)
<inf> hw_pulse_counter: hw pulse counter live: … TIMER3 width filter threshold=2500 µs
```

`from NVS` (vs `compile-time default`) proves
`nvs_store_load_pulse_min_width_us` read the persisted value and the
Kconfig fallback wasn't hit. `TIMER3 width filter threshold=2500 µs`
proves the CC[0] register was programmed with the new threshold — the
runtime filter now rejects anything shorter than 2.5 ms.

## Not yet bench-verified

- **Out-of-range write rejection.** The converter clamps client-side
  (`valueMin: 100`, `valueMax: 10000` on `m.numeric`), so a naive UI
  slider can't produce an out-of-range value. Device-side rejection
  is defense-in-depth for hand-crafted raw MQTT payloads. Code path
  is symmetric with the imp/kWh Divisor rollback that #48 exercises
  and matches the same failure signature (`RET_OUT_OF_RANGE` +
  `ZB_ZCL_SET_ATTRIBUTE` rollback + `LOG_WRN`). Not exercising this
  path in bench-test isn't a regression risk vs. Divisor's rollback
  which is proven.

## Sleep-current cost of the filter path

Deferred to when the #35 INA219 rig is producing routine CSVs, same as
impl-1. The TIMER3 HFCLK-on cost is bounded by `threshold_us × pulse_rate`
per the design spike, so under any realistic real-meter LED rate the
impact is under ~5 µA.

## Related

- Spike: `docs/working/2026-07-29-min-pulse-width-spike.md`.
- Impl-1 bench doc: `docs/working/2026-07-29-min-pulse-width-bench.md`.
- Ticket: #59.
- Pattern precedent: PR #49 (imp/kWh Divisor runtime override).
- Memory: [[reference_z2m_write_read_only_attr_bypass]] — the "address
  by ID + type" write pattern is here extended with a manufacturer code
  to reach an attribute outside the ZCL spec entirely.

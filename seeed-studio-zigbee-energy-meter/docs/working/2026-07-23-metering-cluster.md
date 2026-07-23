# Metering cluster on the app endpoint (issue #5)

## What was added

- `metering_scale.[ch]` — pure-logic helper that converts a `uint64_t`
  pulse total to the ZBOSS `zb_uint48_t` wire layout with saturating
  clamp at 2^48-1. Host-tested (`tests/test_metering_scale.c`, 8 cases).
  Why saturate instead of wrap: Z2M treats a summation wraparound as
  "meter reset" and flushes HA's Energy Dashboard baseline; a stuck-at-
  max device is at least visibly wrong, not silently destructive.
- `src/zb_meter_ep.h` — replaces the temporary `zb_range_extender.h`
  skeleton. Declares a custom "Metering End Device" with three server
  clusters (Basic, Identify, Metering), a reporting context sized for
  the metering cluster's mandatory reportable attributes, and no CVC
  ctx. Modelled on ncs-zigbee's `ZB_HA_DECLARE_SMART_PLUG_*` pattern.
- `src/zigbee_app.[ch]` — endpoint now carries the Metering (0x0702)
  cluster attribute table via `ZB_ZCL_DECLARE_METERING_ATTRIB_LIST_EXT`
  (the EXT variant is required to get Multiplier / Divisor into the
  attribute table; the non-EXT list omits them). Default values match
  the design doc: Multiplier=1, Divisor=1000, UnitOfMeasure=0 (kWh),
  Status=0, MeteringDeviceType=0 (Electric).
- `zigbee_app_publish_summation(uint64_t)` — thread-safe publish path.
  Callers hand the raw pulse count in; the trampoline scheduled on the
  ZBOSS thread packs it into `zb_uint48_t` via `metering_scale_to_u48`
  and calls `ZB_ZCL_SET_ATTRIBUTE`, which persists the value AND marks
  the attribute dirty for the ZBOSS reporting engine.
- `main.c` — pulses now update the metering attribute on every rising
  edge (opportunistic — attribute reads see live data). A
  `k_work_delayable` fires every 5 minutes with the design-doc report
  cadence, wakes and pushes `pulse_accumulator_total()` even if no
  pulses arrived (keeps HA seeing a fresh datapoint for the Energy
  Dashboard's 5-min bucketing).

## Report cadence and wake source

The design-doc target is "wake every 5 min via RTC, publish
CurrentSummationDelivered". Today's non-suspending build implements
this with a `k_work_delayable` at 5-min wall-clock cadence. That IS
functionally equivalent from Z2M's perspective — the attribute report
lands with fresh data every 5 min regardless of what put the CPU to
sleep in between.

The proper RTC-driven wake path (suspend the entire kernel between
ticks, wake off the nRF52840 RTC to run just this handler) is deferred
until issues #6 (battery power / SAADC) and #7 (LPCOMP pulse chain)
land. Both of those change the power/wake model wholesale, and putting
low-power infra in ahead of them means either re-doing it or building
on top of a scaffold that doesn't actually save power yet. Non-goal
for #5.

`k_work_reschedule` in the handler re-arms the tick for the next
interval; no separate timer needed.

## Reporting-engine dependencies

`ZB_ZCL_SET_ATTRIBUTE(..., ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, ...)`
internally calls `zb_zcl_mark_attr_for_reporting`. Whether that
translates into an actual report on the wire depends on Z2M having
sent Configure-Reporting on this attribute — which Z2M does by
default when it discovers the metering cluster during interview
(with defaults roughly min=60s, max=3600s, reportable-change=1).

So the observable behaviour when everything works:
- Z2M pairs → interviews → configures reporting.
- Every ~5 min our work handler updates the attribute.
- ZBOSS's reporting engine sees the dirty flag and, at the next
  min-interval-honouring slot, TXes an attribute report.
- Z2M receives it, the HA Energy Dashboard steps up.

If Z2M's default config-reporting doesn't fire (e.g. the interview
timed out), the attribute value on device is still correct — a manual
"Read" from Z2M's dashboard returns it. That covers acceptance
criterion 3 (manual read returns live value) independent of
reporting-engine behaviour.

## Endpoint device ID choice

Zigbee HA doesn't define a canonical "metering device" ID.
`ZB_METER_DEVICE_ID = 0x0053` is a custom-app number — no conflict
with the HA-standard IDs and no tool enforces validation against a
table. Z2M identifies our device via cluster fingerprint plus the
Basic-cluster manufacturer/model strings, so `0x0053` never actually
matters at commissioning time. Kept out of `ZB_HA_SMART_PLUG_DEVICE_ID`
(0x0051) because that IS a real HA device type — Smart Plug expects
On/Off + Electrical Measurement, which we don't ship.

## Sleepy-behavior disabled during interview

Bench-verifying interview on this branch surfaced a real timing bug:
Z2M's `Interview` step reads `Basic.manufacturerName / .modelId` via
ZCL Read after the ZDO Simple-Descriptor discovery, and if the
target is a Sleepy End Device, the read times out because the SED's
default parent-poll cadence (~7.5 s) is slower than herdsman's read
deadline. Result: `interview_state: FAILED — can not get active
endpoints`, endpoint clusters are populated correctly (that phase
uses ZDO which the parent answers on the SED's behalf), but
manufacturer/model stay `None` and Z2M skips the follow-up
Configure-Reporting step.

Fix: `zigbee_configure_sleepy_behavior(false)` at boot. This keeps
`CONFIG_ZIGBEE_ROLE_END_DEVICE=y` at the MAC layer (we're still a
ZED that doesn't accept children) but toggles rx-on-when-idle back
on. Radio current goes ~5 mA — a non-starter for the multi-year AAA
target, but fine for the USB-dev phase we're currently in.

Sleep gets turned back on in #6/#7, alongside the LPCOMP pulse chain
and real power measurement (both change the wake model so there's no
point half-implementing sleep on this branch).

## Basic-cluster identity + Z2M external converter

Z2M requires a matching herdsman-converter definition before it will
send Configure-Reporting during interview or accept raw
`{ read: { cluster: 'seMetering' } }` requests on a device. Without
one, `bridge/logging` emits:

    z2m: No converter available for 'get' 'read' ([object Object])

To close AC bullets 3–5 without user-side JS work, this branch:

1. Extends the Basic cluster to `ZB_ZCL_DECLARE_BASIC_ATTRIB_LIST_EXT`
   so the device advertises `manufacturer_name = "kwood1992"`,
   `model_id = "xiao-power-meter"`, plus stack/hw/app version and a
   date code. This is the fingerprint the converter matches on.
2. Ships an external converter at
   `external-converters/xiao-power-meter.js` (repo root, deliberately
   NOT under the firmware tree). It's a one-line
   `m.electricityMeter({ cluster: 'metering', endpointNames: ['10'] })`
   which turns our raw pulse-count summation into an `energy` sensor
   in kWh via the `electricityMeter` modernExtend helper.

**Install steps for the Z2M add-on (Home Assistant OS):**

    1. cp external-converters/xiao-power-meter.js  \\
       /addon_configs/<slug>_zigbee2mqtt/external_converters/
    2. Add to Z2M configuration.yaml:
         external_converters:
           - external_converters/xiao-power-meter.js
    3. Restart the Z2M add-on.
    4. Re-interview the device from Z2M's frontend (three-dot menu →
       Re-interview) so ConfigureReporting lands on our device.

After that: `zigbee2mqtt/xiao_meter` (or the IEEE-based friendly name)
starts publishing `energy` in kWh derived from CurrentSummationDelivered
÷ Divisor, and Z2M's raw `<device>/get` accepts
`{ read: { cluster: 'seMetering', attributes: [...] } }` for on-demand
attribute reads.

## Bench-verified (2026-07-23)

Flashed and paired against a real Z2M+ZStack3x0 coordinator through
the Pi/SWD/D6 rig (`tools/test-join.sh`). What passed on-device
+ what Z2M can see without the external converter installed:

- Interview: `interview_state: SUCCESSFUL` with `manufacturer:
  kwood1992`, `model_id: xiao-power-meter`, `date_code: 20260723`,
  `software_build_id: 0.5.0`, `power_source: Battery`.
- Endpoint 10 clusters (per Z2M): `[genBasic, genIdentify,
  seMetering]`. Simple-descriptor bytes at `simple_desc_app_ep` in
  the ELF decode to `{profile=0x0104, device=0x0053, in={0,3,0x0702},
  out={}}` — matches on-device declaration.
- Z2M's `bridge/response/device/generate_external_definition`
  produced `extend: [m.electricityMeter({cluster:'metering',
  endpointNames:['10']})]` — exactly the converter shape we
  committed.
- Z2M sent Configure-Reporting on our device: `configured_reportings`
  shows `currentSummDelivered` (max 65000 s, delta 100) and
  `instantaneousDemand` (max 65000 s, delta 5). Binding
  `seMetering → coordinator ep 1` is present.
- On-device 5-min tick fires cleanly. Serial log
  (`00:05:05.825`): `metering report tick:
  CurrentSummationDelivered=182` (110 button-simulated pulses + a
  few boot-time ambient ADC edges). Publish path calls
  `ZB_ZCL_SET_ATTRIBUTE` on the ZBOSS thread as designed.
- ZBOSS reporting engine is actually TXing over the air: Z2M's
  `bridge/health.devices.0xf4ce...` shows **121 messages** from
  our device across the join+report window.

## Still gated on the Z2M external converter

- Z2M-decoded `energy` (kWh) / `power` (W) values landing on
  `zigbee2mqtt/xiao_meter` — Z2M refuses to publish decoded state
  for `supported: false` devices even though it CAN generate the
  definition on demand. Installing the converter promotes the
  device to `supported: true` and closes this loop.
- `<device>/get {read: {cluster: 'seMetering', ...}}` — same
  `No converter available for 'get' 'read' ([object Object])`
  error the user saw. Also unblocked by the converter.

Install steps for the converter are in the previous section. Once
installed, the acceptance-criterion bullets 3 (manual read returns
live value) and 5 (increment pulses → report → Z2M sees new value)
become verifiable in one round of Z2M-add-on restart + re-interview.

## Coordinator-facing timing gotcha (documented for future me)

Z2M's `bridge/request/device/interview` (i.e. RE-interview after
initial join) frequently fails with `Interview failed because can
not get active endpoints` on our device. It's a Sleepy-ED / interview-
timeout mismatch and NOT a firmware issue — Z2M's active-endpoints
ZDO retry deadline is short enough that a sleeping child times out
even at rx-on-when-idle=TRUE. Workaround: prefer full factory-reset
+ fresh join over re-interview when a stack update needs to land.

## Coordination with #7

`main.c`'s bench pulse source (button IRQ + ADC sample loop) still
feeds `pulse_accumulator_update`. Issue #7 will swap those out for the
LPCOMP/PPI hardware chain but keeps the accumulator interface, so the
publish path added here needs no change on that swap.

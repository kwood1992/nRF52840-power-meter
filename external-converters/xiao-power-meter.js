// Z2M external converter for the XIAO nRF52840 Zigbee Energy Meter.
//
// Install steps live in the repo README under
// "Home Assistant / Zigbee2MQTT setup".
//
// Uses ES-module syntax (`import` / `export default`) — that's what
// Z2M 2.x's external-converter loader parses.
//
// Matches on `zigbeeModel: ['xiao-power-meter']` — same key Z2M's
// auto-generated definition uses when it detects the electricityMeter
// pattern, so an external converter that overrides the auto-gen must
// match on the same field for Z2M's definition-lookup precedence to
// pick it over the generated one. (An earlier revision matched on
// `fingerprint: [{endpoints: [{ID: 10, inputClusters: [...]}]}]`.
// Fingerprint matching does work in principle for Z2M's built-in
// converter library, but for external converters overriding an
// auto-generated definition on the SAME device, zigbeeModel is what
// wins the tiebreak. Confirmed on 2026-07-23 against Z2M 2.12.1.)
//
// The zigbeeModel string comes from the on-device Basic-cluster
// `model_id` attribute (set by `BASIC_MODEL_ID` in zigbee_app.c).
// If you rename the model in firmware, this string must move in
// lockstep or the converter stops matching.
//
// Exposes the Metering (0x0702) cluster's CurrentSummationDelivered as
// an `energy` sensor in kWh (Multiplier=1, Divisor=1000 are applied by
// the modernExtend electricityMeter helper, matching what the design
// doc sets on-device).

import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

// seMetering.Divisor addressed by numeric ID + type. The ZCL spec
// marks Divisor read-only, so zigbee-herdsman-converters' generic
// write path pre-checks the attribute *name* against that spec and
// rejects the write with NOT_AUTHORIZED before it hits the wire —
// even though our firmware exposes Divisor with an app-flipped
// READ_WRITE access flag (see #48). Passing `attribute` as
// `{ID, type}` bypasses that name-based precheck: modernExtend's
// numeric() then writes `{770: {value, type: 0x22}}` on the wire,
// which the ZHC lookup never touches. Same object drives fromZigbee
// (parses the divisor attribute out of readResponse / attributeReport)
// and the interview-time read.
const DIVISOR_ATTR = {ID: 0x0302, type: 0x22};  // 770 / uint24

// Manufacturer-specific min-pulse-width filter threshold in µs
// (issue #59). The attribute lives in the same Metering (seMetering)
// cluster as Divisor but carries a Nordic Semiconductor manufacturer
// code (0x1015). Pass manufacturerCode alongside {ID, type} on the
// attribute descriptor AND in zigbeeCommandOptions — belt-and-braces
// so both interview-time reads and user-initiated writes tag the
// frame with the mfr code and the on-device attribute lookup finds
// our manufacturer-specific slot instead of returning UNSUPPORTED_ATTRIBUTE.
const NORDIC_MANUF_CODE = 0x1015;
const MIN_PULSE_WIDTH_ATTR = {
    ID: 0xF000,
    type: 0x21,          // 61440 / uint16
    manufacturerCode: NORDIC_MANUF_CODE,
};

export default {
    zigbeeModel: ['xiao-power-meter'],
    model: 'xiao-power-meter',
    vendor: 'kwood1992',
    description: 'XIAO nRF52840 Zigbee pulse-counting power meter (issue #5)',
    extend: [
        m.electricityMeter({
            // The device's Metering cluster carries CurrentSummationDelivered
            // as the raw pulse count. Multiplier=1, Divisor=1000 are
            // written to the cluster's own attributes so Z2M / HA read
            // kWh natively; no need to override here.
            cluster: 'metering',
            // No `endpointNames` — this device has a single endpoint
            // (10) so we want the plain `energy` / `power` properties,
            // not the `_10`-suffixed variants. With `endpointNames`
            // set, modernExtend defines `property: 'energy_10'` in the
            // exposes AND publishes both `energy` and `energy_10` to
            // MQTT — but only the plain one gets the value. The Z2M
            // frontend reads via the `property` field, so exposes
            // would show NA even while the real value is being
            // published. Confirmed 2026-07-24 on bench.
        }),
        // Editable meter-constant field. Surfaces the runtime-writable
        // Divisor from #48 as `imp_per_kwh` on the device's Exposes tab
        // (and as an HA number entity) so installers don't have to
        // hand-craft the numeric-ID MQTT payload.
        //
        // `reporting: false` skips setting up periodic attribute
        // reporting for Divisor — the value only changes on explicit
        // user write, and the firmware doesn't advertise reporting for
        // this attribute. A one-shot read still happens at interview
        // time (access includes GET) so the current value populates.
        //
        // Range mirrors the firmware's `calibration_is_valid_imp_per_kwh`
        // predicate (see src/calibration.h) and the Kconfig range on
        // CONFIG_APP_METERING_DEFAULT_IMP_PER_KWH — out-of-range writes
        // are additionally rejected + rolled back on-device, but
        // range-clamping in the UI is the friendlier first line.
        //
        // Sleepy-ED gotcha: this device polls its parent every 60 s
        // in steady state and Z2M's write deadline is 10 s. Writes
        // land only inside the ~30 s post-reboot turbo-poll window;
        // otherwise they time out at the herdsman layer. Documented
        // in the README's "Sleepy-ED timing gotcha" section — a
        // wake-on-write path is future work (see #50 follow-ups).
        m.numeric({
            name: 'imp_per_kwh',
            cluster: 'seMetering',
            attribute: DIVISOR_ATTR,
            description:
                'Meter pulse constant (imp/kWh). Match this to the value '
                + "printed on your meter's faceplate. Persists in on-device "
                + 'NVS across reboots. Requires the device to be awake — '
                + 'short-press the button before writing if it has been '
                + 'idle >30 s.',
            valueMin: 100,
            valueMax: 10000,
            valueStep: 1,
            access: 'ALL',
            reporting: false,
            entityCategory: 'config',
        }),
        // Min-pulse-width filter (issue #59). Rejects LPCOMP crossings
        // shorter than this threshold on the hardware chain (LPCOMP UP
        // → TIMER3 timer → TIMER2 count), keeping ambient flicker /
        // torch-driver PWM out of the count. Default 1000 µs; range
        // 100-10000 µs enforced by the firmware's calibration predicate.
        //
        // Sleepy-ED write timing: same gotcha as imp_per_kwh — press
        // the button first if the device has been idle.
        m.numeric({
            name: 'min_pulse_width_us',
            cluster: 'seMetering',
            attribute: MIN_PULSE_WIDTH_ATTR,
            zigbeeCommandOptions: {manufacturerCode: NORDIC_MANUF_CODE},
            description:
                'Minimum pulse width in microseconds. Pulses shorter than '
                + 'this are rejected in hardware before the count register '
                + 'increments — protects against ambient flicker and torch '
                + 'PWM. Default 1000 µs; typical residential meter LEDs '
                + 'emit pulses in the 20–100 ms range so any legal value '
                + "can't miss real imp events. Persists in NVS.",
            valueMin: 100,
            valueMax: 10000,
            valueStep: 1,
            access: 'ALL',
            reporting: false,
            entityCategory: 'config',
        }),
    ],
};

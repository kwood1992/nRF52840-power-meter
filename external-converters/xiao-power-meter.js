// Z2M external converter for the XIAO nRF52840 Zigbee Energy Meter.
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
//
// Install (Home Assistant OS + Z2M add-on):
//   1. Copy this file to /homeassistant/zigbee2mqtt/external_converters/
//      (the older `/config`-based Z2M layout) OR to
//      /addon_configs/<slug>_zigbee2mqtt/external_converters/ on newer
//      installs. Check where /homeassistant/zigbee2mqtt/configuration.yaml
//      lives — the external_converters/ dir goes next to it.
//   2. Enable the converter in Z2M's configuration.yaml. The path is
//      relative to Z2M's config dir, not absolute:
//        external_converters:
//          - external_converters/xiao-power-meter.js
//   3. Restart the Z2M add-on.

import * as m from 'zigbee-herdsman-converters/lib/modernExtend';

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
    ],
};

/*
 * Zigbee2MQTT external converter for the XIAO nRF52840 pulse-counting
 * power meter.
 *
 * WHY THIS LIVES IN THE REPO
 * --------------------------
 * The converter and the firmware are one interface in two halves. A
 * firmware change to the cluster list or an attribute does nothing
 * user-visible until the converter agrees, and a converter that mentions
 * an attribute the firmware no longer publishes leaves a permanently-null
 * sensor in Home Assistant. Keeping this file next to the firmware means
 * the pair can be reviewed and versioned together; keeping it only on the
 * Z2M host means the next cluster change silently half-lands.
 *
 * DEPLOYING
 * ---------
 * Z2M reads it from `external_converters/xiao-power-meter.js` relative to
 * its data directory (confirmed in this deployment's bridge/info). Copy
 * it there and restart Z2M:
 *
 *     cp z2m/xiao-power-meter.js <z2m-data>/external_converters/
 *     # restart Zigbee2MQTT
 *
 * A cluster-list change also needs the device removed and re-paired —
 * Z2M caches the endpoint's cluster list by IEEE and a device-side
 * factory reset does not invalidate it. `tools/test-join.sh` does that
 * removal as step 3.
 *
 * Targets Z2M 2.12.x (ESM external converters).
 */

import {Zcl} from 'zigbee-herdsman';
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';
import * as reporting from 'zigbee-herdsman-converters/lib/reporting';
import fz from 'zigbee-herdsman-converters/converters/fromZigbee';
import tz from 'zigbee-herdsman-converters/converters/toZigbee';

const e = exposes.presets;
const ea = exposes.access;

/*
 * Manufacturer-specific min-pulse-width filter threshold, µs.
 * Attribute 0xF000 on seMetering under Nordic's member ID 0x1015 — see
 * the matching defines in src/zigbee_app.c.
 */
const MIN_PULSE_ATTR = 0xf000;
const MANUF_CODE = 0x1015;

/* The firmware declares a single endpoint (APP_ENDPOINT in zigbee_app.c). */
const APP_ENDPOINT = 10;

const fzLocal = {
    /*
     * Divisor doubles as the imp/kWh calibration knob (#48), and 0xF000
     * is the pulse-width filter (#59). Both arrive on seMetering.
     *
     * herdsman surfaces an attribute it has no name for under its numeric
     * ID, but which numeric form depends on the cluster definition, so
     * accept both the hex and decimal spellings rather than guessing.
     */
    xiao_metering_config: {
        cluster: 'seMetering',
        type: ['attributeReport', 'readResponse'],
        convert: (model, msg) => {
            const result = {};
            if (msg.data.divisor !== undefined) {
                result.imp_per_kwh = msg.data.divisor;
            }
            const pulseWidth = msg.data[MIN_PULSE_ATTR] ?? msg.data['61440'];
            if (pulseWidth !== undefined) {
                result.min_pulse_width_us = pulseWidth;
            }
            return result;
        },
    },
};

const tzLocal = {
    imp_per_kwh: {
        key: ['imp_per_kwh'],
        convertSet: async (entity, key, value) => {
            /*
             * Addressed by name here because Divisor is a standard
             * attribute. Note the firmware hand-rolls its descriptor to
             * make it READ_WRITE — the stock ZBOSS one is read-only and
             * the write would come back NOT_AUTHORIZED.
             */
            await entity.write('seMetering', {divisor: value});
            return {state: {imp_per_kwh: value}};
        },
        convertGet: async (entity) => {
            await entity.read('seMetering', ['divisor']);
        },
    },
    min_pulse_width_us: {
        key: ['min_pulse_width_us'],
        convertSet: async (entity, key, value) => {
            await entity.write(
                'seMetering',
                {[MIN_PULSE_ATTR]: {value, type: Zcl.DataType.UINT16}},
                {manufacturerCode: MANUF_CODE},
            );
            return {state: {min_pulse_width_us: value}};
        },
        convertGet: async (entity) => {
            await entity.read('seMetering', [MIN_PULSE_ATTR], {manufacturerCode: MANUF_CODE});
        },
    },
};

export default {
    zigbeeModel: ['xiao-power-meter'],
    model: 'xiao-power-meter',
    vendor: 'kwood1992',
    description: 'XIAO nRF52840 Zigbee pulse-counting energy meter',

    fromZigbee: [fz.metering, fz.battery, fzLocal.xiao_metering_config],
    toZigbee: [tzLocal.imp_per_kwh, tzLocal.min_pulse_width_us],

    exposes: [
        e.energy(),

        /*
         * `power` is deliberately absent. A pulse counter cannot produce
         * an honest instantaneous demand: between two pulses it knows
         * only "no pulse yet", so any W figure reads as zero under light
         * load and spikes when a pulse lands. The firmware no longer
         * declares InstantaneousDemand at all. Derive power in HA from
         * the kWh series with a derivative helper, per the design doc.
         */

        e.battery(),
        e.battery_voltage(),

        exposes
            .numeric('imp_per_kwh', ea.ALL)
            .withValueMin(100)
            .withValueMax(10000)
            .withDescription(
                'Meter pulses per kWh (Metering Divisor). Persisted in device NVS and ' +
                    'survives a factory reset, because it describes the physical meter ' +
                    'rather than the pairing.',
            ),

        exposes
            .numeric('min_pulse_width_us', ea.ALL)
            .withUnit('us')
            .withValueMin(100)
            .withValueMax(10000)
            .withDescription(
                'Reject LPCOMP crossings shorter than this, to filter LED flicker and ' +
                    'electrical noise. Persisted in device NVS.',
            ),
    ],

    configure: async (device, coordinatorEndpoint) => {
        const endpoint = device.getEndpoint(APP_ENDPOINT);

        await reporting.bind(endpoint, coordinatorEndpoint, [
            'genBasic',
            'genPowerCfg',
            'seMetering',
        ]);

        await reporting.readMeteringMultiplierDivisor(endpoint);
        await reporting.currentSummDelivered(endpoint, {min: 60, max: 3600, change: 1});

        /*
         * Battery: the device reports once per 5-minute tick, so a max
         * interval of ~1 h with change:1 (0.5 %) is plenty and keeps the
         * radio quiet. Wrapped because a sleepy ED can miss the
         * ConfigureReporting window; a failure here must not abort the
         * rest of configure() and leave the device half-set-up.
         */
        try {
            await reporting.batteryPercentageRemaining(endpoint, {
                min: 3600,
                max: 62000,
                change: 1,
            });
        } catch {
            /* reporting engine will retry on the next interview */
        }

        /*
         * Explicit reads so the exposes have values immediately after
         * pairing instead of sitting null until something happens to
         * write them.
         *
         * min_pulse_width_us in particular is manufacturer-specific, and
         * Z2M does not read those during a standard interview — that is
         * why it showed as null before this read existed. The firmware
         * has always populated the attribute at init.
         */
        await endpoint.read('genPowerCfg', ['batteryVoltage', 'batteryPercentageRemaining']);
        await endpoint.read('seMetering', [MIN_PULSE_ATTR], {manufacturerCode: MANUF_CODE});
    },
};

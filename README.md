# nRF52840 Power Meter

Firmware for a battery-powered Zigbee sensor that watches the pulse LED on a
household electricity meter, counts the flashes, and reports cumulative energy
(kWh) to Home Assistant via Zigbee2MQTT every 5 minutes.

Built on the **Seeed XIAO nRF52840** with the nRF Connect SDK (Zephyr) and the
ncs-zigbee add-on. The pulse pipeline runs entirely in hardware peripherals
(LPCOMP → PPI → TIMER) during System-ON sleep, targeting multi-year AAA
battery life.

## Status

Early development. Pure-logic core (pulse accumulator with 32-bit HW-wrap and
48-bit overflow handling) is done and host-tested. Zephyr walking-skeleton
compiles and boots to a USB CDC-ACM heartbeat log. Zigbee stack, pulse-source
wiring, and low-power paths are next.

See [`seeed-studio-zigbee-energy-meter/docs/working/`](seeed-studio-zigbee-energy-meter/docs/working/)
for current in-flight status.

## Documentation

- **[Design doc](seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md)** — locked decisions, BOM, firmware structure, testing strategy, and open risks. Read this first.
- **[Contributing / dev environment setup](CONTRIBUTING.md)** — cross-platform toolchain install, build, flash, and host-test workflow.
- **[Working notes](seeed-studio-zigbee-energy-meter/docs/working/)** — living status snapshots.

## Home Assistant / Zigbee2MQTT setup

Once the firmware is flashed and the device has joined your Zigbee2MQTT
coordinator, install the external converter so Z2M decodes the Metering
cluster's `CurrentSummationDelivered` as an `energy` sensor in kWh (which
the HA Energy Dashboard consumes directly).

The converter is [`external-converters/xiao-power-meter.js`](external-converters/xiao-power-meter.js).

### 1. Copy the converter into Z2M's config dir

Z2M looks for external converters next to its `configuration.yaml`.
Where that lives depends on your install:

- **Home Assistant OS + Z2M add-on (recent versions)** —
  `/addon_configs/<slug>_zigbee2mqtt/external_converters/`
- **Home Assistant OS + Z2M add-on (older `/config` layout)** —
  `/homeassistant/zigbee2mqtt/external_converters/`
- **Standalone Z2M** — the `external_converters/` sub-directory of
  whatever you passed as `ZIGBEE2MQTT_DATA` (defaults to Z2M's `data/`).

Create the `external_converters/` directory if it doesn't exist, then
copy `xiao-power-meter.js` into it.

### 2. Register the converter

Edit `configuration.yaml` (the Z2M one, not HA's) and add:

```yaml
external_converters:
  - external_converters/xiao-power-meter.js
```

The path is relative to Z2M's config directory, not absolute.

### 3. Restart Z2M and re-interview

Restart the Z2M add-on (or process). From the Z2M frontend, open the
device and pick *Re-interview* from the three-dot menu — this makes Z2M
send Configure-Reporting under the new converter definition.

### 4. Verify

The device's *Exposes* tab should show **Energy (kWh)** with a live
value; after a few pulses it should tick upward. In Home Assistant,
**Settings → Dashboards → Energy → Add consumption** should list the new
`sensor.<name>_energy` entity as a valid grid-consumption source.

If Energy Dashboard still says "no valid sources," give it a few minutes
of accumulated statistics — HA rejects entities with zero history.

## License

[GNU GPL](LICENSE).

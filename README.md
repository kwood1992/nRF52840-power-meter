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

## Calibrating imp/kWh

The firmware ships with **Divisor = 1000 imp/kWh** (`Multiplier = 1`),
matching the design-doc default. If your meter's pulse LED is rated
differently — most electricity meters print the value on the faceplate
next to a marking like `1000 imp/kWh`, `800 imp/kWh`, or the equivalent
`Wh/pulse` — you need to change the Divisor to match. The new value is
persisted in on-device NVS and survives reboots; no reflash needed.

Valid range: **100–10000 imp/kWh**. Writes outside that range are
silently rolled back (the readback tells the truth).

### MQTT (copy-pasteable)

Publish to `zigbee2mqtt/<friendly_name>/set`:

```json
{"write":{"cluster":"seMetering","payload":{"770":{"value":800,"type":34}}}}
```

- `770` = 0x0302 = ZCL attribute ID for `Divisor`. **Address by numeric
  ID, not by the name `divisor`** — zigbee-herdsman-converters pre-checks
  the named form against the ZCL spec's access flags and refuses the
  write, because the standard marks Divisor as read-only. The numeric
  form skips that lookup and reaches the device.
- `34` = 0x22 = ZCL data type for uint24 (Divisor's on-wire type).
- Replace `800` with your meter's imp/kWh.

Read back to confirm:

```json
{"read":{"cluster":"seMetering","attributes":["divisor"]}}
```

Read is fine to address by name — only the write path is affected by
the ZHC precheck.

### Z2M Dev Console

Open the device in the Z2M frontend → **Dev console** tab → **Write
attribute**. Cluster = `seMetering`, Attribute = `770` (numeric — for
the same reason as above), Type = `uint24`, Value = your imp/kWh.

### Sleepy-ED timing gotcha

The device is a sleepy end-device polling its parent every 60 s in
steady state. A single write from the UI will time out with a
herdsman `Timeout after 10000ms` error unless the device is inside
its **post-reboot turbo-poll window** (roughly 30 s after any power-on
or reset). Practical workflow:

1. Short-press the device button (triggers a fresh join → turbo-poll
   window opens).
2. Send the write within the next ~30 s.
3. Read back to confirm.

Re-flashing over SWD or double-tap-resetting the board has the same
effect as the button press. During a battery-mode deployment where
neither is convenient, retrying the write until one lands within a
poll window is the current workaround. A proper "wake on write" path
(#TODO — see follow-up issue) is future work.

### Changing Divisor mid-life

The persisted accumulator (`CurrentSummationDelivered`) is **not**
re-scaled when Divisor changes. If the device has been counting at
`Divisor=1000` for a week and you switch to `Divisor=800`, the raw
pulse total keeps growing on its old baseline but Z2M now divides by
800, so the reported kWh reading jumps proportionally. In practice
this only matters if you're calibrating against a running meter — do
the calibration on first install, then leave it alone.

## License

[GNU GPL](LICENSE).

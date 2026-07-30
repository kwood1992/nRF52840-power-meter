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
silently rolled back on-device (the readback tells the truth); the
converter also range-clamps in the UI as a first line.

### Z2M UI (recommended)

The external converter exposes `imp_per_kwh` as an editable numeric
field. Open the device in the Z2M frontend → **Exposes** tab, edit
the *Imp per kwh* field, hit save. In Home Assistant the same field
appears as a number entity under the device
(`number.<name>_imp_per_kwh`), settable from a dashboard card or the
`number.set_value` service.

Read the sleepy-ED timing gotcha below before writing — a UI save
that lands outside the wake window will error with
`Timeout after 10000ms`, and the on-device value won't change.

### MQTT (copy-pasteable fallback)

Same primitive, if you'd rather script it:

```json
{"imp_per_kwh": 800}
```

Publish to `zigbee2mqtt/<friendly_name>/set`. The converter turns that
into a raw ZCL write addressed by numeric ID (770 / uint24) so it
bypasses zigbee-herdsman-converters' spec-based read-only precheck for
Metering.Divisor. If you ever need to skip the converter entirely
(e.g. debugging), the underlying wire form is:

```json
{"write":{"cluster":"seMetering","payload":{"770":{"value":800,"type":34}}}}
```

- `770` = 0x0302 = ZCL attribute ID for `Divisor`. Address by numeric
  ID, not by the name `divisor` — the named form gets rejected client-
  side because the ZCL spec marks Divisor read-only.
- `34` = 0x22 = ZCL data type for uint24.

Read back to confirm:

```json
{"read":{"cluster":"seMetering","attributes":["divisor"]}}
```

Read is fine to address by name — only the write path is affected by
the ZHC precheck.

### Sleepy-ED timing gotcha

The device is a sleepy end-device polling its parent every 60 s in
steady state, while Z2M gives up on a write after 10 s.

Since the Poll Control (0x0020) cluster shipped, **you usually don't
have to do anything about this.** The device advertises `genPollCtrl`,
which makes zigbee-herdsman *queue* a write instead of failing it at the
10 s deadline, then deliver it the next time the device makes contact.
In practice that's the next 5-minute metering report, so an unattended
write — including one from a Home Assistant automation, with nobody at
the device — lands within about 5 minutes.

Two knobs govern the fallback path: `CONFIG_APP_ZIGBEE_CHECKIN_INTERVAL_S`
(default 900 s) is the backstop for when reporting is idle, and
`CONFIG_APP_ZIGBEE_FAST_POLL_TIMEOUT_S` (default 20 s) is how long the
device fast-polls once Z2M says it has something queued.

If you want a write to land *now* rather than within a few minutes, wake
the device first:

1. **Short-press the device button.** The white LED acks the press and
   a ~30 s turbo-poll window opens (~100 ms poll cadence).
2. Send the write within that window.
3. Read back to confirm.

The short press is join-state aware: on an **already-joined** device it
only refreshes the poll window and leaves the network state, bindings
and attributes untouched. On an **unjoined** device it still starts
network steering, as before — so pairing is unchanged.

Window length is `CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS` (default 30 s).
It costs nothing in steady state: turbo poll only runs when you ask for
it, and ZBOSS reverts to the long-poll interval when the window expires.

Re-flashing over SWD or double-tap-resetting the board also opens a
window (via the post-join path), but a short press is the cheap option
since it doesn't disturb the join.

> **If you change the firmware's cluster list, remove and re-pair the
> device in Z2M.** Z2M serves endpoint/cluster data from its own
> database, and a device-side factory reset does *not* make it re-read
> the simple descriptor — it will keep reporting the old cluster list
> and report a successful interview while doing so.

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

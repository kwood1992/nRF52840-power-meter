# Battery Zigbee Power-Meter Pulse Counter

## Investigate and Grill on using ESPHome to implement code & use Zigbee protocol

* https://botmonster.com/smart-home/esphome-zigbee-nrf52-end-device/
* https://esphome.io/components/zigbee/
* https://esphome.io/components/nrf52/

## Context

Build a wireless sensor that watches the flashing pulse LED on a household
electricity meter, counts the flashes, and reports a cumulative energy reading
to Home Assistant over Zigbee every 5 minutes — running unattended on AAA
batteries for years.

**Why the design differs from the original one-liner.** The starting idea was
"Seeed Zigbee board + LDR + deep-sleep + count flashes on AAA." Grilling exposed
hard conflicts that reshaped it:

- **LDR + deep-sleep is a contradiction.** A passive LDR can't be read while the
  CPU is off; catching every flash needs a *hardware* wake/count path.
- **ESP32 + AAA + per-flash counting drains fast** (~months at best) because a
  full-SoC wake every report is expensive. Battery was declared mandatory.
- Chosen resolution (informed, eyes-open): **XIAO nRF52840**, whose hardware
  peripherals count pulses while the CPU sleeps (~2–3 µA), reaching multi-year
  life. Accepted cost: the Zephyr / ncs-zigbee **R23** stack is the harder,
  vendor-discouraged toolchain (see Risks).
- **LDR → phototransistor.** Meter LED is confirmed **visible red**, but the LDR
  is too slow at peak load; a phototransistor (µs response) removes undercount
  risk. The kit's LDR is not used.

## Decisions (locked)

| Area | Decision |
|---|---|
| MCU | Seeed **XIAO nRF52840** (plain, not Sense) |
| Firmware | **nRF Connect SDK (Zephyr)** + **ncs-zigbee R23** add-on, role = Sleepy End Device (`ZB_ZED`) |
| Sensor | **Phototransistor** (visible/IR), load resistor, light-sealed snout |
| Counting | Phototransistor → on-chip **LPCOMP** → **PPI** → **TIMER (counter mode)**, all in hardware during System-ON sleep, zero CPU wakes |
| Report | Wake via RTC timer every **5 min**, send **cumulative** count |
| Zigbee model | **Metering cluster 0x0702** `CurrentSummationDelivered`, `Multiplier=1`, `Divisor=1000` (imp/kWh, adjustable) → HA reads **kWh** natively. Plus **Power Config 0x0001** `BatteryVoltage` + `BatteryPercentageRemaining` → Z2M `battery` / `voltage`. **No `InstantaneousDemand`** — see below |
| Power | **2×AAA series = 3.0 V** direct to BAT/VDD (1.7–3.6 V range). No boost/LDO. Uses kit spring terminals in a printed compartment |
| Coordinator | **Zigbee2MQTT** + custom external converter |
| Enclosure | 3D-printed all-in-one, opaque phototransistor snout, **3M foam-tape** mount (split sensor-head = documented fallback) |
| Extras | Tactile button (join/factory-reset), battery % via SAADC + Power Config cluster, min-pulse-width noise filter |

## Hardware / BOM (with AU purchase links)

| Part | Recommended pick | Where to buy (AU first) | ~Price |
|---|---|---|---|
| **MCU** | Seeed **XIAO nRF52840** (plain, pre-soldered) — *not* Sense/Plus | [Core Electronics SS102010631](https://core-electronics.com.au/seeed-studio-xiao-nrf52840-pre-soldered-bluetooth-5-0-ble-wireless-iot-microcontroller-board.html) · [Pakronics](https://www.pakronics.com.au/products/seeed-studio-xiao-nrf52840-pre-soldered-ss102010631) · AliExpress: Seeed official store (cheaper, slow ship) | ~$25 AUD |
| **Phototransistor** | **Vishay TEPT4400** — visible-light, human-eye response (ideal for a red meter LED) | [Little Bird — Photo Transistor Light Sensor](https://littlebirdelectronics.com.au/products/photo-transistor-light-sensor) · [Core Electronics CE09800](https://core-electronics.com.au/phototransistor.html) | ~$1–4 |
| Load resistor | ~10–100 kΩ (tune for clean LPCOMP swing) | Jaycar/Altronics resistor pack (generic) | ~$1 |
| **Battery** | 2×AAA — **use the Bambu kit spring terminals** in the printed compartment (leads to BAT pads). *Easier ready-made fallback:* 2×AAA holder w/ switch + JST | Fallback: [Core Electronics — 2×AAA holder w/ switch + JST (ADA4191)](https://core-electronics.com.au/2-x-aaa-battery-holder-with-on-off-switch-jst-ph-connector.html) · [Altronics S5055](https://www.altronics.com.au/p/s5055-2-x-aaa-battery-holder-with-on-off-switch/) | kit / ~$3 |
| AAA cells | **Energizer Ultimate Lithium AAA** (best longevity + cold/voltage hold) | Any supermarket / Amazon AU | ~$10/4pk |
| **Button** (join/reset) | Mini SPST tactile, PCB/breadboard | [Core Electronics — Pololu 5-pack](https://core-electronics.com.au/mini-pushbutton-switch-pcb-mount-2-pin-spst-50ma-5-pack.html) · [Jaycar SP0611](https://www.jaycar.com.au/spst-pcb-tactile-switch/p/SP0611) | ~$1–5 |
| Mount tape | 3M VHB / foam mounting tape (light-sealing) | Jaycar / Bunnings (generic) | ~$5 |
| Enclosure | 3D-printed (your Bambu), light-sealing phototransistor snout | self-printed | — |

Notes: flashing is **UF2 over USB** (double-tap reset) or SWD pads — no debugger
needed to load firmware (J-Link/RTT optional for debug). Zigbee **coordinator
assumed already owned** (you run Z2M); not in BOM. Avoid the Jaycar ZD1950
phototransistor — it's IR-only and your meter LED is visible red.

## Firmware structure (Zephyr)

Base on a Nordic Zigbee sample (nearest metering/sensor sample) and add:

1. **Pulse pipeline** — devicetree + `nrfx` config for `LPCOMP` (analog in from
   phototransistor node, reference from the ladder) → PPI channel → TIMER in
   counter mode. Runs in System-ON sleep; RAM retained. Read the counter at
   report time; maintain a 48-bit monotonic accumulator (handle TIMER wrap +
   NVS-persist across resets/battery change).
2. **Metering cluster server** — register endpoint, set `Multiplier`/`Divisor`,
   push `CurrentSummationDelivered`. *(R23 specifics verified against live docs.)*
3. **Sleepy End Device** — `ZB_ZED`, long sleep, 5-min RTC wake to report +
   sample battery (SAADC on VDD) → Power Configuration cluster.
4. **Commissioning** — button-triggered network steering / factory reset.
5. **Noise filter** — reject LPCOMP crossings shorter than a min pulse width.

## Zigbee2MQTT integration

- External converter (`.js`) defining device fingerprint (modelID/manufacturer)
  and exposing the Metering cluster as an **energy (kWh)** sensor. Lives in
  the repo at `external-converters/xiao-power-meter.js` and must be deployed to the Z2M host's
  `external_converters/` — it is half of the device interface and has to move
  in step with any firmware cluster change.
- In HA: kWh → Energy Dashboard; power (W) via HA derivative helper.

### No instantaneous power attribute (decided 2026-07-30)

`InstantaneousDemand` is deliberately not published, and the converter exposes
no `power`. A pulse counter cannot produce an honest instantaneous demand:
between two pulses it knows only "no pulse yet", so any W figure divides by an
open-ended interval — reading zero under light load and spiking when a pulse
lands. In practice the exposed `power` sensor sat at 0 permanently, which is
worse than absent because HA graphs it.

All three of `InstantaneousDemand`, `DemandFormatting` and
`HistoricalConsumptionFormatting` are optional in SE 1.4, so dropping them is
spec-legal. Derive power from the kWh series with the HA derivative helper, as
the row above already specified.
- Calibration = change `Divisor` (no reflash) if meter isn't 1000 imp/kWh.

## Testing (TDD — write tests first, per project rules)

Pure logic is host-testable and gets tests **before** implementation:

- **Ztest / native_sim (Zephyr):** counter→summation scaling, `Divisor`
  application, 48-bit rollover, accumulator persistence across reset. Target 80%+
  on this logic layer.
- **Z2M converter:** unit-test in JS (fed synthetic Metering attribute reports →
  assert exposed kWh) before wiring the real device.
- Hardware paths (LPCOMP/PPI/radio) can't be unit-tested — verified on-bench.

## Verification (end-to-end)

1. **Bench pulse source** — blink an LED at a *known* rate (signal gen or a
   spare MCU) into the phototransistor; confirm counted total matches over N
   pulses and at peak-rate (≈4 Hz) with no undercount.
2. **Zigbee** — pair to Z2M; confirm the device exposes a kWh sensor and the
   summation increments; confirm HA Energy Dashboard ingests it.
3. **Power** — measure sleep current (multimeter / Power Profiler II); confirm
   low-µA sleep and project battery life.
4. **Ambient light** — verify no false counts with the snout under room light /
   torch; adjust threshold/shroud.
5. **Field** — mount on the meter; over 24 h compare device kWh delta to the
   meter's own reading; tune `Divisor` if off.

## Risks / open items

- **ncs-zigbee R23 maturity** — exact west-manifest entries, Kconfig symbols,
  Metering-cluster API, sample layout to be **verified against current Nordic
  docs during build** (my memory not trusted here). Highest-friction area.
- **LPCOMP ambient sensitivity** — depends on shroud quality + threshold; foam
  snout is the mitigation.
- **imp/kWh unknown** — defaulted to 1000, adjustable via `Divisor`.
- **Mount** — defaulted to all-in-one foam tape; revisit after inspecting the
  meter face (split head is the fallback).

# Hardware — pinout, wiring, and build guide

Everything you need to build the sensor yourself: which pins the firmware
claims, how to wire the phototransistor and battery, what the LEDs mean,
and where the SWD pads are when you need to recover a board.

If you only want to *flash* a pre-built firmware, you need none of this —
see the [README quick start](../README.md). This document is for people
assembling the physical device or changing the pin assignments.

---

## Board variant

> **If you're buying a board today, get the Sense.** Both variants compile,
> but every bench measurement and join test in this repo was taken on a
> Sense. The plain board has never been run.

| Variant | Board target for `west build -b` | Status |
|---|---|---|
| XIAO nRF52840 **Sense** | `xiao_ble/nrf52840/sense` | **Supported.** The bench rig board; mounts as `XIAO-SENSE` in bootloader mode. |
| XIAO nRF52840 (plain) | `xiao_ble/nrf52840` | **Builds, untested on hardware.** CI compiles it every PR, but nobody has flashed one. The design doc's BOM specifies this variant, so it should work — if you run one, please report back. |

The two boards are electrically equivalent for this project's purposes — the
Sense just adds an IMU and a PDM microphone, neither of which the firmware
uses. There is no pinout difference on any pin this project touches, which is
why the plain board is expected to work.

> **Do not remove the IMU-disable block in `overlays/sense.overlay`.** The
> Sense board DTS declares the LSM6DS3TR-C's regulator with
> `regulator-boot-on`, so Zephyr powers the IMU at boot even though nothing
> uses it. It then draws its ~0.6–1.7 mA quiescent current forever, which
> swamps the entire sleep budget. The overlay disables the sensor node, its
> regulator, and the mic regulator.
>
> Those nodes exist only on the Sense, which is why they live in
> `overlays/sense.overlay` rather than `app.overlay` — the latter is loaded
> for every target, and referencing them there is what used to break the
> plain build ([#75](https://github.com/kwood1992/nRF52840-power-meter/issues/75)).
> `CMakeLists.txt` appends the file when the board target ends in `sense`,
> and CI asserts against the generated devicetree that all three nodes really
> are disabled — because if that overlay were ever silently skipped, the
> build log would look perfectly clean while the sleep budget quietly
> doubled.

---

## Pin assignments

Every pin the firmware touches. XIAO silkscreen names on the left, nRF52840
port/pin in the middle — that's what the devicetree and `nrfx` calls use.

### Signal pins

| XIAO pin | nRF52840 | Direction | Used for | Defined in |
|---|---|---|---|---|
| **A0** / D0 | P0.02 (AIN0) | Analog in | **Phototransistor input.** LPCOMP compares it against VDD×3/8 with hysteresis on. | `app.overlay` (`&comp`) |
| **D6** | P1.11 | Input, pull-up, active-low | **User button** — join / wake / factory reset | `app.overlay` (`user_button`) |
| **D7** | P1.12 | Input, pull-up, active-low | **Bench pulse injection** only. Not populated on a real build. | `src/hw_pulse_counter.c` |

### Onboard LEDs (no wiring needed)

All three are **active-low** on the XIAO — the devicetree carries the
`GPIO_ACTIVE_LOW` flag, so firmware code uses logical values (1 = lit).

| Colour | nRF52840 | DT alias |
|---|---|---|
| Red | P0.26 | `led0` |
| Green | P0.30 | `led1` |
| Blue | P0.06 | `led2` |

### Power

| XIAO pin | Connect to | Notes |
|---|---|---|
| **3V3** | 2×AAA pack **+** | This is the SoC VDD rail directly. See [Battery wiring](#battery-wiring) — this choice is load-bearing. |
| **GND** | 2×AAA pack **−** | |
| BAT pads | *(not used)* | Wiring cells here instead breaks battery reporting — see below. |

### Peripherals claimed internally

Useful if you're adding features and need to know what's already taken.

| Peripheral | Claimed by | Why not another instance |
|---|---|---|
| LPCOMP (`&comp`) | Pulse detect on AIN0 | Only one LPCOMP on the part |
| TIMER2 | Pulse counter (counter mode) | TIMER0 = radio driver, TIMER1 = 802.15.4 driver |
| TIMER3 | Min-pulse-width gate | TIMER0/1/2 all taken |
| GPIOTE | D7 bench inject path | Shared with Zephyr's GPIO driver via the nrfx channel allocator |
| SAADC ch 7 | Battery voltage (`NRF_SAADC_VDD`) | Internal VDD tap — not pin-mapped, so no conflict with AIN0 |
| UARTE0, TWIM0 | **Deliberately disabled** | Left enabled they request PCLK16M, pinning HFCLK at ~1.5 mA |

---

## Sensor wiring — the phototransistor

The sensor watches the meter's pulse LED. A phototransistor is specified
rather than an LDR because an LDR's response time (tens of ms) can miss
pulses at peak load; a phototransistor responds in microseconds.

**Part:** Vishay TEPT4400 or similar *visible-light* phototransistor. Do
not substitute an IR-only part (e.g. Jaycar ZD1950) — domestic meter pulse
LEDs are typically visible red, and an IR part will not see them.

### Circuit

```
        3V3 ──────┐
                  │
                 ─┴─  phototransistor (TEPT4400)
                 \ /   collector to 3V3
                 ─┬─   emitter to A0
                  │
    A0 ───────────┼──────────► to nRF52840 P0.02 / AIN0
                  │
                 ┌┴┐
                 │ │  R_load  10k–100k
                 └┬┘
                  │
        GND ──────┘
```

Light on the phototransistor pulls A0 **up**; darkness lets `R_load` pull
it down. The firmware's LPCOMP threshold is fixed at **VDD × 3/8** — about
1.24 V on a 3.3 V bench rail, 1.125 V on a 3.0 V battery pack.

### Choosing `R_load`

Pick the value that puts the LED-on level comfortably above the threshold
and the LED-off level comfortably below it:

- **Too small** → the swing never reaches the threshold, pulses are missed.
- **Too large** → ambient light alone holds A0 above the threshold, and
  the pulses vanish into a permanently-high signal.

Start at 47 kΩ. If you have a scope or a multimeter, measure A0 with the
snout on the meter, LED dark and LED lit, and confirm the two levels
straddle the threshold with margin. Without instruments, the pragmatic
method is to flash a red LED at the snout and watch the pulse count in Z2M.

### Light sealing matters more than the resistor

The single biggest source of false counts is ambient light leaking into
the snout. The enclosure should present an opaque tube pressed against the
meter face, sealed with foam tape. The firmware's defences are secondary:

- **LPCOMP hysteresis** (`enable-hyst`) rejects jitter right at the
  threshold with no CPU wake.
- **Minimum pulse width** (`CONFIG_APP_PULSE_MIN_WIDTH_US`, default
  1000 µs) discards crossings shorter than a real meter pulse. Real imp
  pulses are 20–100 ms, so there is a wide margin. This exists because
  bench testing with an LED torch produced 3–4× overcounting: the torch's
  PWM flicker read as "steady on" to the eye but as a pulse train to
  LPCOMP.

If you get overcounting in the field, fix the light seal before reaching
for the pulse-width knob.

---

## Battery wiring

**2×AAA in series → the XIAO's `3V3` pin and `GND`.** No boost, no LDO.

The nRF52840 runs from 1.7 V to 3.6 V, so a 2-cell pack (3.0 V nominal)
feeds it directly. Energizer Ultimate Lithium (L92) cells are the
recommended chemistry — they hold voltage flat for most of their life and
tolerate cold, both of which matter for a meter box.

> **The `3V3` pin, not the `BAT` pads.** This is not interchangeable.
>
> The firmware reads battery voltage through the SoC-internal SAADC VDD
> tap (`NRF_SAADC_VDD`), which measures whatever rail the chip is running
> from. Wire the cells to `3V3` and that rail *is* the pack, so the
> reading is the pack voltage. Wire them to `BAT` instead and the onboard
> regulator holds VDD at a fixed rail — battery percentage then reads
> ~100% forever regardless of how flat the cells are.
>
> Using the BAT pads would mean re-doing the ADC path against Seeed's
> onboard divider (P0.31 / AIN7, gated by driving P0.14 **low**). Zephyr's
> `xiao_ble` board definition declares none of that, so it's on you.

> **3.6 V is an absolute maximum for the nRF52840, not a guideline.** A
> *fresh* lithium AAA sits near 1.8 V open-circuit, so two in series can
> land at or just above 3.6 V before any load is applied. This is a known
> and accepted margin for this design, but it is why the ADC full scale is
> set to 3.6 V and should not be widened — there is nothing meaningful to
> measure above the part's own limit.

**Never leave USB connected while the board is also powered from an
external supply on the 3V3 rail.** Two supplies through different
regulators in parallel is not a safe steady state. For flashing an
externally-powered board, use the SWD path (`tools/flash-swd.sh`), which
doesn't touch USB.

### Battery percentage

Reported percentage is a linear map between two Kconfig values, not a real
discharge curve:

| Option | Default | Meaning |
|---|---|---|
| `CONFIG_APP_BATTERY_FULL_MV` | 3000 | Reported as 100% |
| `CONFIG_APP_BATTERY_EMPTY_MV` | 2000 | Reported as 0% |

A linear map is deliberate — a lithium AAA curve is flat enough that no
simple approximation does better. Treat the number as a "replace soon"
indicator, not a fuel gauge. Uncalibrated SAADC accuracy is ±3% typical,
which over that 1 V span is roughly ±10 percentage points.

---

## The user button

Wire a momentary SPST tactile switch between **D6** and **GND**. The pin
has an internal pull-up and reads active-low, so no external resistor is
needed.

### Gestures

| Gesture | Duration | Effect |
|---|---|---|
| **Short press** | < 1 s | *Not joined:* start network steering (pair the device).<br>*Already joined:* open a ~30 s turbo-poll window so a Z2M attribute write lands immediately. Does **not** disturb the join. |
| *(dead zone)* | 1–3 s | **Nothing.** Deliberate gap so a fumbled press neither joins nor wipes the device. |
| **Long press** | > 3 s | **Factory reset** — leaves the Zigbee network and clears state. Works regardless of join state, because a device the user believes is stuck is exactly when reset has to work. |
| **Hold during boot** | > 3 s from power-on | **Erase the stored energy accumulator** (resets the kWh total to 0). Separate from factory reset — this keeps the network join. |

Thresholds are `BUTTON_SHORT_MAX_MS` / `BUTTON_LONG_MIN_MS` in
`src/main.c`. The classifier itself is pure logic and host-tested in
`tests/test_button_press_classifier.c`.

---

## LED reference

The device is **silent by default on battery** — no LED activity except in
response to a user gesture or a fault. The boot flash and the 5-minute
report heartbeat are compile-time options (`CONFIG_APP_BOOT_FLASH`,
`CONFIG_APP_REPORT_HEARTBEAT`), both off unless you build with `dev.conf`.

| Pattern | Looks like | Means |
|---|---|---|
| Button ack | Single 100 ms white flash | Press registered |
| Long-press hold | Solid red while held | You are 3 s from a factory reset — release now to abort |
| Joining | Blue, blinking | Network steering in progress |
| Join success | Green | Joined |
| Join fail | Red | Steering failed |
| Erase confirm | Flash after boot-hold | Accumulator zeroed |
| Heartbeat | 50 ms green every 5 min | Report tick (`dev.conf` builds only) |
| **Fatal** | *N* red flashes per 5 s cycle | Init failure — count the flashes |

### Fatal flash codes

| Flashes | Failure |
|---|---|
| 1 | CDC-ACM device not ready |
| 2 | `usb_enable()` failed |
| 3 | Button configuration failed |
| 4 | Pulse counter init failed |
| 5 | Button IRQ arm failed |

After 10 minutes the fatal pattern drops to a single flash every 10 s. A
broken device in a meter box needs its indicator to survive until someone
notices it — 10% duty over a fortnight would flatten the pack first.

---

## SWD pads

Five pads on the **underside** of the XIAO. You need them only for
recovering a soft-bricked board or for RTT logging with USB detached —
normal flashing is UF2 over USB with no debugger.

| Pad | Purpose |
|---|---|
| SWDIO | Serial Wire data |
| SWCLK | Serial Wire clock |
| RST | Hardware reset |
| GND | Common ground — **mandatory** |
| 3V3 | Power. **Leave disconnected** if the board is already powered over USB. |

Seeed publishes a *Bottom-pad-positioning* drawing with exact locations;
the pads are also silkscreened.

Full wiring, probe options, and the recovery procedure are in
**[swd-recovery-jig.md](swd-recovery-jig.md)**.

### Flash regions you must never write

The XIAO ships with the Adafruit UF2 bootloader. Two regions are its
territory, and overwriting either destroys double-tap recovery — the board
goes dark and only SWD gets it back.

| Range | Contents |
|---|---|
| `0x00000000`–`0x00027000` | MBR + SoftDevice |
| `0x000F4000`–`0x00100000` | Adafruit UF2 bootloader |

The application is linked at `0x27000` via `CONFIG_USE_DT_CODE_PARTITION`.
If you add flash-backed storage, place it inside the app slot. This
project bricked its first board by putting `zboss_nvram` at `0xF4000`.

---

## Bench testing without a meter

`D7` exists so the rig can inject synthetic pulses without any optics. Pull
it **low** to simulate a pulse — it feeds the same TIMER3 width gate and
TIMER2 counter as the real LPCOMP path, so it exercises the filter honestly.

The helper scripts in [`tools/`](../tools/) drive it from a Raspberry Pi
(`xiao-pulse.sh`, `xiao-pulse-burst.sh`, `xiao-pulse-us.sh`). You can just
as well ground the pin by hand with a jumper.

> **Two caveats when measuring current during pulse tests.** Holding D7 low
> sinks about +0.242 mA through the pin's internal pull-up, which is a
> *bench artifact and not a firmware cost* — subtract a duty-matched
> control run or you will overstate pulse cost by roughly 6×. And don't
> calibrate against an LED torch: PWM flicker causes 3–4× overcounting.
> Use the D7 GPIO path for 1:1 counting.

---

## Related documents

- **[Design doc](../seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md)** — locked decisions, BOM with purchase links, and the rationale for each choice
- **[SWD recovery](swd-recovery-jig.md)** — un-bricking, probe wiring, one-command flash rig
- **[CONTRIBUTING](../CONTRIBUTING.md)** — toolchain setup and build
- **[tools/](../tools/)** — bench scripts

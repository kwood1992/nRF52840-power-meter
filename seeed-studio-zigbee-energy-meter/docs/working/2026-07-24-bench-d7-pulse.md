# 2026-07-24 — Dedicated D7 bench pulse-simulator (#16)

## Why this exists

Surfaced during #5's bench verification: injecting pulses via
`~/xiao-short-press.sh` (which pulls Pi GPIO 17 → D6 LOW for 250 ms)
doubled as a Zigbee-join re-attempt on every press because the D6 ISR
served two masters — it bumped `bench_pulse_count` on the falling edge
AND handed the release duration to the button classifier, which then
called `zigbee_app_start_join()`. That meant a `~/xiao-short-press.sh`
burst of 110 pulses to cross Configure-Reporting's delta=100 threshold
spammed `bridge/logging` and the serial console with

    button short-press (250 ms) — joining
    zigbee_app_start_join: bdb_start returned TRUE
    network steering signal: 0 (already joined)

per pulse. Small radio-thread wake per no-op join, but mostly a
diagnosability problem — real Z2M signals got buried.

## What changed

**Firmware (`src/main.c`)**
- `user_button_isr` (D6) no longer touches `bench_pulse_count`. It only
  latches the press-start timestamp and, on release, publishes the
  duration to the classifier thread. Pure join / factory-reset now.
- New `pulse_input_isr` (D7 = P1.12) — falling-edge only, atomic
  increment of `bench_pulse_count`, no other side effects. Zero
  debounce: `~/xiao-pulse.sh` generates a clean ~250 ms LOW window,
  well above any GPIO noise floor, and any real EMI-driven jitter
  would still be under the LOW window, so a debounce here would only
  drop legit injections during a burst.
- `pulse_input_setup()` mirrors `user_button_setup()` using
  `GPIO_INT_EDGE_TO_ACTIVE` (falling-edge in DT active-low terms).
- Removed the now-dead `bench_last_edge_ms` / `BENCH_DEBOUNCE_MS`
  scaffolding — the debounce was only there to bound button-bounce
  spam, and there's no button on D7.

**DT (`app.overlay`)**
- New `pulse_input` gpio-keys node on `&gpio1 12` (D7) with
  `GPIO_ACTIVE_LOW | GPIO_PULL_UP`, alias `sw1`.

**Pi rig (`docs/swd-recovery-jig.md`)**
- New "Optional bench-input wires" subsection documenting both the
  existing GPIO 17 → D6 wire and the new GPIO 27 → D7 wire.

**Pi helper templates (`tools/xiao-pulse.sh`, `tools/xiao-pulse-burst.sh`)**
- Reference scripts to copy into the Pi's home dir. `xiao-pulse.sh`
  pulses GPIO 27 LOW for 250 ms; `xiao-pulse-burst.sh` loops it N times
  with a configurable gap.

## Wiring

Pi header pin 13 (BCM GPIO 27) → female Dupont → XIAO front-side D7 pad.
Ground is already shared through the SWD block, so a single wire.

## Bench session ergonomics

Before:

```
$ for i in $(seq 1 110); do ssh rpi-xiao '~/xiao-short-press.sh'; sleep 0.5; done
[...110× "button short-press (250 ms) — joining" in the serial log...]
```

After:

```
$ ssh rpi-xiao '~/xiao-pulse-burst.sh 110'
sent 110 pulse(s) on GPIO 27 -> XIAO D7
```

Serial log stays quiet during the burst; `bench_pulse_count` climbs
silently; the sample loop's edge poll picks it up on the next tick.

## Follow-ups deferred

- `~/z2m-cli pulses N` wrapper (issue #16 AC bullet): lives on the Pi
  alongside the existing `z2m-cli`, so not committed here. Suggested
  body: `ssh $host "~/xiao-pulse-burst.sh $2"` — combines with the
  existing `z2m-cli sub` for a one-shot bench cycle.
- LPCOMP hardware chain (issue #7): retires `bench_pulse_count` and the
  entire software pulse path. This scaffolding disappears then.

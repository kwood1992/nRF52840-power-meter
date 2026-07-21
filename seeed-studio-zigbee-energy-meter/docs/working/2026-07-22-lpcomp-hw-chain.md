# Hardware pulse counting via LPCOMP + PPI + TIMER (issue #7)

## What was added

- `pulse_source_hw.[ch]` — LPCOMP → PPI → TIMER2 counter chain. Runs
  during System-ON sleep with the CPU asleep, catching every pulse
  without a wake.
- `lpcomp_ref.[ch]` — pure-logic helper mapping "desired trigger mV @
  known VDD" to the nearest LPCOMP internal-reference step on the
  VDD/16 ladder. Host-tested (`tests/test_lpcomp_ref.c`, 6 cases).
- `main.c` refactor — swaps the sample loop's data source from
  ADC + software `pulse_edge_detector` to `pulse_source_hw_count()`.
  Sample cadence relaxed from 10 Hz to 1 Hz since no pulses are lost
  between reads.
- `app.overlay` — removed the `zephyr,user` ADC channel node
  (LPCOMP owns AIN0 at runtime without a DT binding).
- `prj.conf` — dropped `CONFIG_ADC`, added `CONFIG_NRFX_LPCOMP`,
  `CONFIG_NRFX_PPI`, `CONFIG_NRFX_TIMER`, `CONFIG_NRFX_TIMER2`.
- Deleted `pulse_edge_detector.[ch]` and its host test — the module
  has no callers now.

## Chain topology

```
Phototransistor (AIN0 = P0.02)
        │
        ▼
     LPCOMP  ──[UP event]──► PPI ──► TIMER2 COUNT task
     (VDD/16 ladder,                  (counter mode, 32-bit)
      hysteresis on)                          │
                                              ▼
                                    read on wake via
                                    nrfx_timer_capture()
```

Everything from AIN0 → TIMER2 stays live during `k_sleep()`. The CPU
only wakes at the 1 Hz sample cadence (soon: 5 min once #5 lands the
Zigbee report loop) to snapshot the counter and hand it to the
accumulator.

## Min-pulse-width filter

**Not implemented in this slice.** First-pass uses LPCOMP's hardware
hysteresis (~50 mV band) as the noise mitigation. That handles jitter
around the threshold but doesn't reject a fast glitch that fully
crosses the band.

`pulse_source_hw.c` has a comment sketching the two-TIMER hardware
approach for the follow-up:

```
  TIMER-measure: shorts { COMPARE0 → STOP + CLEAR }, capture 0 =
                 min-pulse-width in µs
  PPI-A: LPCOMP UP    → TIMER-measure START
  PPI-B: LPCOMP DOWN  → TIMER-measure STOP + CLEAR
  PPI-C: TIMER-measure COMPARE0 → TIMER-count COUNT
```

Filter runs entirely in hardware during sleep — no CPU wake. Uses
3 PPI channels and 1 additional TIMER. Left for a bench session to
size N against real ambient-vs-pulse timing.

## Threshold choice

`main.c` sets `PHOTOTRANSISTOR_THRESHOLD_MV = 1000 mV` and
`VDD_NOMINAL_MV = 3000 mV`. That routes through `lpcomp_choose_ref_step_16`
to step 5/16 (~937 mV). The design doc's earlier "1000 mV midpoint"
guidance for the ADC path carries over; expect to tune on the bench
by watching the boot log's `LPCOMP threshold=... → step X/16 (~Y mV)`
line and moving the constant if pulses under-count.

## What was NOT verified

- **No bench verification.** The entire acceptance checklist that
  needs a signal generator (1000+ pulses @ ≥4 Hz), a Power Profiler
  (µA-range sleep current), and a real phototransistor is untouched.
  Static verification done: host tests pass (18 cases); nrfx API
  usage matches the driver conventions for LPCOMP / PPI / classic
  TIMER on nRF52840; TIMER2 assumption documented so a mid-review
  reader can spot if ncs-zigbee actually claims it.
- **`west build` not run** — no toolchain in this sandbox. The
  first build against ncs-zigbee (once #3 lands) will validate the
  Kconfig combinations.

## Coordination

- Depends on #4 (this branch is stacked on `afk/issue-4-zigbee-join`).
  The button IRQ scaffolding #4 introduced stays intact; this PR
  just deletes the bench-pulse-counting call inside it.
- Interacts with #2 (NVS persist) at the accumulator level only —
  `pulse_source_hw_count()` returns a 32-bit hw counter that feeds
  `pulse_accumulator_update()`, whose downstream persistence path
  is unchanged.
- Once #5 (Metering cluster + 5-min reports) lands, the sample loop
  will move from 1 Hz polling to an RTC-driven 5-min wake — the
  hw counter can go longer than that between reads without loss.

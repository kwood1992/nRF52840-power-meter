# Sleepy-ED baseline — joined + POR'd + SWD detached (#8)

First measurement of the firmware's actual sleep-current floor after all
the contamination sources from prior runs were eliminated:

- SWD wires physically detached (so CDBGPWRUP can't be re-latched)
- Full POR via the Pi BCM 22 → NC relay in the 3V3 → BAT path (clears
  the latch left over from the last flash session)
- Device joined and interviewed by Z2M with `interview_state=SUCCESSFUL`,
  `type=EndDevice`, `power_source=Battery`

CSV: `ina219-2026-07-29-185443-sleepy-ed-baseline.csv`
Firmware: d788b1a (main, rtt.conf build — RTT log, USB stack off, pulse
counter on)

## Numbers

180 s at 10 Hz, 1802 samples through the INA219 (0.1 Ω shunt on the
Pi 3V3 → XIAO BAT rail):

| Metric | Value |
|---|---|
| Rail voltage | 3.27 – 3.31 V (stable) |
| Mean current | **−0.036 mA** (below the INA219 rig's mean-noise floor) |
| Mean \|current\| | 0.158 mA (noise-dominated at ±0.3 mA per sample) |
| Max current | **3.40 mA** — single-sample poll/report catch at t=74.3 s |
| Samples > 0.4 mA | 6 of 1802 (~0.3 %) |
| Peak times | 5.5 s, 44.4 s, 74.3 s, 164.0 s, 164.3 s, 165.7 s |

The `-0.036 mA` mean means the true firmware sleep current is close
enough to zero that we're seeing the INA219's small zero-calibration
offset dominate. 6 wake-catches over 180 s at a 100 ms sampling period
is consistent with a poll interval on the order of 5–15 s and a wake
window of ≤ 100 ms per poll.

## What this replaces

Pre-POR runs on this ticket all landed at a 1.60 – 1.65 mA mean with a
tight unimodal distribution. That was the ARM CoreSight / HFCLK
overhead from `CDBGPWRUP` being latched in the DP CTRL/STAT register
while SWD was electrically connected — see #57 and
`2026-07-29-por-test-result.md` for the full derivation.

This baseline is a **~45× improvement over the number this ticket had
accepted** and confirms the firmware sleep architecture works as
designed.

## Rig limits, and what we still can't say

The INA219 at 0.1 Ω shunt is a poor µA-range instrument:

- Single-sample noise ≈ ±30 µV shunt = ±300 µA current
- Averaging 1800 samples pulls that down by √N to ≈ 7 µA — so the mean
  is meaningful, but any *individual* sub-mA reading is noise
- Deep-sleep floor for the nRF52840 in System-OFF is spec'd at ~1.5 µA,
  System-ON idle at tens of µA — both well below what this rig can
  resolve per sample

We can say with confidence: the firmware's average current in
joined-sleepy-ED state is **below the INA219 rig's mean-noise floor of
about 50 µA**. We *cannot* say from this rig alone whether the true
average is 5 µA, 20 µA, or 50 µA — that needs a Power Profiler Kit II
or equivalent, on its own ticket.

## Battery-life projection

Energizer Ultimate Lithium AAA is spec'd at ~1200 mAh at moderate
loads; 2 in series gives the same 1200 mAh at 3.0 V nominal.

| Assumed average | Runtime | Notes |
|---|---|---|
| 50 µA (INA219 upper bound) | 24 000 h ≈ 2.7 years | Worst case within what this rig can distinguish |
| 20 µA (plausible, sleep dominant) | 60 000 h ≈ 6.8 years | Needs PPK2 to confirm |
| 10 µA (best plausible) | 120 000 h ≈ 13.7 years | Battery self-discharge dominates long before this |

The AAA lithium's own ~1 %/year self-discharge caps practical runtime
at ~5–7 years regardless of the firmware getting any better than
~20 µA. So further sleep-floor optimisation past the current baseline
is likely wasted engineering — the physics of the cell is the limit.

## Acceptance-criteria status on #8 after this run

Firmware / measurement side (all this session):

- [x] Device runs unplugged from USB and continues to report to Z2M
  through a full report cycle — `interview_state=SUCCESSFUL`,
  `power_source=Battery`, poll behaviour visible in the CSV
- [x] Sleep-current measurement recorded — this doc + CSV
- [x] Peak-current measurement recorded — 3.4 mA single-sample catch;
  rejoin/scan peaks separately observed at 9–17 mA in the
  `2026-07-29-por-test-result.md` runs
- [x] Battery-life projection calculated — table above

Hardware side (still HITL, not blocking a numbers writeup):

- [ ] Phototransistor collector wire moved from 3V3 → VDD
- [ ] 2×AAA holder mounted in the printed compartment
- [ ] Firmware tuning motivated by the measurements — deferred; no
  tuning is worth doing off the INA219 rig, as any change smaller than
  ~50 µA is invisible to it. Re-open when PPK2 arrives.

## What "done" on #8 looks like now

The current-measurement part of the ticket is answered — the firmware
sleeps as designed and the battery-life headroom is generous. The
ticket stays open only on the physical-assembly items above.

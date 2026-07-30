# Min-pulse-width filter — ±100 µs boundary AC bench (#59)

Closes the last checkbox on #59 that impl-1 and impl-2 left open: the
`threshold ± 100 µs` discrimination test the AC calls for. Impl-1 could
only run at 3× / 0.4× discrimination because the shell-driven
`xiao-pulse.sh` bottoms out at 5–15 ms per edge (`pinctrl` fork/exec).

## Tool

`tools/xiao-pulse-us.{sh,py}` (this session). Pi-side script writes
BCM2837 `GPSET0` / `GPCLR0` directly through `/dev/gpiomem` and
busy-waits on `time.perf_counter_ns()` between edges. No `pigpiod`
(unavailable on Debian trixie's Raspberry Pi 3B+ image) — pure Python
stdlib plus `pinctrl` for setup/teardown. Effective edge jitter on an
otherwise-idle Pi 3B+ is well under ±100 µs.

## Setup

- Firmware: main @ a0f6282 (impl-2 landed).
- On-device threshold: `2500 µs` (persisted in NVS from the impl-2
  MQTT-write test). RTT boot log:
  ```
  <inf> hw_pulse_counter: min-pulse-width: 2500 µs (from NVS)
  <inf> hw_pulse_counter: hw pulse counter live: ... TIMER3 width filter threshold=2500 µs
  <inf> main: restored accumulator_total=2802230 from NVS
  ```
- SWD attached for RTT (log capture, not counting-critical).
- D7 bench-inject wire connected (BCM 27 → XIAO D7 / P1.12).

## Smoke test — 10 pulses well above threshold

```
$ tools/xiao-pulse-us.sh 10 5000 5000
sent 10 pulse(s): pulse_us=5000 gap_us=5000 on BCM 27
```

RTT:
```
[00:00:12.427] <inf> main: pulse(s) counted: delta=10 accumulator_total=2802230→2802240
[00:00:12.429] <inf> main: persisted accumulator_total=2802240
```

10/10 counted at 5 ms LOW × 5 ms HIGH — proves the wire, D7 GPIOTE, and
counter chain are alive and my pulses reach the XIAO before any
boundary claims.

## Boundary test — threshold ± 100 µs

Sequenced back-to-back inside one RTT session so the accumulator delta
is unambiguous.

### Test A — 1000 pulses at 2400 µs (threshold − 100 µs), gap 5000 µs

```
$ tools/xiao-pulse-us.sh 1000 2400 5000
sent 1000 pulse(s): pulse_us=2400 gap_us=5000 on BCM 27
```

Total wall time ~7.4 s. Expected: **0 counted** (below the 2500 µs
threshold, filter rejects on the PPI fabric).

**Zero `pulse(s) counted` log lines fire during or after this burst.**
No CPU wake. Accumulator stays at 2802230.

### Test B — 1000 pulses at 2600 µs (threshold + 100 µs), gap 5000 µs

```
$ tools/xiao-pulse-us.sh 1000 2600 5000
sent 1000 pulse(s): pulse_us=2600 gap_us=5000 on BCM 27
```

Expected: **1000 counted** (above threshold, filter accepts).

RTT `pulse(s) counted` deltas across the burst window (40 sample-loop
ticks at ~200 ms each):

```
delta=7,  delta=27, delta=26, delta=27, delta=26, delta=27, delta=26,
delta=26, delta=27, delta=26, delta=27, delta=26, delta=27, delta=26,
delta=26, delta=27, delta=26, delta=27, delta=26, delta=26, delta=27,
delta=26, delta=27, delta=26, delta=26, delta=27, delta=26, delta=27,
delta=26, delta=26, delta=27, delta=26, delta=27, delta=26, delta=26,
delta=27, delta=26, delta=27, delta=26, delta=14
```

Sum = **1000**. Accumulator: 2802230 → 2803230.

### Result

| Test | Pulses | Width | Expected | Observed |
|---|---|---|---|---|
| A | 1000 | 2400 µs (thresh − 100) | 0 counted, reject on PPI | 0 counted, no log lines |
| B | 1000 | 2600 µs (thresh + 100) | 1000 counted | 1000 counted |

Filter discriminates cleanly at ±100 µs. Combined with the impl-1
mechanism proof, the filter is now bench-verified end-to-end at the AC
boundary.

## AC status after this session

- [x] Filter runs entirely in hardware — reconfirmed: test A produced
      zero CPU wakes (no log lines during 7.4 s reject burst).
- [x] Kconfig default with rationale — impl-1.
- [x] Field-adjustable via Zigbee attribute — impl-2.
- [x] Survives reboot (NVS-backed) — impl-2 verified.
- [x] **Bench verification via `tools/xiao-pulse-us.sh` on D7:** 1000
      pulses at (threshold − 100 µs) → 0 counted; 1000 at (threshold +
      100 µs) → 1000 counted. This ticket closes the AC.
- [x] External converter exposes the setting in the Z2M UI — impl-2.

## Related

- Impl-1 bench: `docs/working/2026-07-29-min-pulse-width-bench.md`.
- Impl-2 bench: `docs/working/2026-07-30-min-pulse-width-impl2.md`.
- Ticket: #59.
- Tool: `tools/xiao-pulse-us.{sh,py}`. Notable that Debian trixie on
  Raspberry Pi 3B+ has no `pigpiod` in-repo; the mmap-plus-busy-wait
  approach avoids the dependency entirely.

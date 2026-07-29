# Min-pulse-width filter — impl-1 bench verification (#59)

Follow-up to the design spike at
`docs/working/2026-07-29-min-pulse-width-spike.md`. Implements the Option A
all-hardware filter per the spike's recommendation, plus routes the D7
bench-inject path through the same TIMER3 gate so the AC bench test
exercises the filter uniformly.

## What impl-1 lands

- `CONFIG_APP_PULSE_MIN_WIDTH_US` Kconfig (int, default 1000, range
  100–10000) — production compile-time threshold. Runtime NVS-backed
  override + Zigbee manufacturer-specific attribute are deferred to
  impl-2; the field-adjust story from the ticket body isn't wired yet.
- TIMER3 enabled in `app.overlay` + `CONFIG_NRFX_TIMER3=y` in `prj.conf`.
- `src/hw_pulse_counter.c` rewired per the spike: LPCOMP UP/DOWN and D7
  GPIOTE HITOLO/LOTOHI both feed TIMER3 START/CLEAR + STOP/CLEAR via
  PPI forks; TIMER3 CC[0] = threshold with a SHORTS_COMPARE0_STOP so
  the timer freezes once the threshold is crossed and only PCLK16M is
  requested while a pulse is in flight; TIMER3 CC[0] event → TIMER2
  COUNT completes the chain.

Two GPIOTE channels for D7 (HITOLO + LOTOHI); nrfx's `pin_flags`
bookkeeping only tracks the last-configured channel, so both are
enabled directly at the HAL level (`nrf_gpiote_event_enable`) rather
than through `nrfx_gpiote_trigger_enable(pin, …)`.

## Bench verification

All three tests use `xiao-pulse.sh <LOW_ms>` on the Pi (D7 = XIAO
P1.12) with the count read from `pulse(s) counted` RTT log lines.
Bursts run under a temporary Kconfig override so the threshold sits
above the Pi userland `sleep` + `pinctrl` fork/exec overhead (~5–15 ms)
where the shell can't produce a clean discrimination.

### Test 1 — production default, normal-width pulses

- Threshold: **1000 µs** (Kconfig default; no bench overlay).
- Burst: 10 pulses at `xiao-pulse.sh` default (250 ms LOW).
- Expected: **all count** (250 ms ≫ 1 ms threshold).

Result: 10/10 counted. Accumulator: 2802206 → 2802216. Log excerpt:

```
threshold=1000 µs
pulse(s) counted: delta=1 accumulator_total=2802207
pulse(s) counted: delta=1 accumulator_total=2802208
… × 8 more, each delta=1 …
pulse(s) counted: delta=1 accumulator_total=2802216
```

Confirms the filter path preserves normal operation — no regression on
production-default builds.

### Test 2 — 50 ms threshold, well-above-threshold pulses

- Threshold: **50 000 µs** (`CONFIG_APP_PULSE_MIN_WIDTH_US=50000`,
  Kconfig range temporarily widened for the test).
- Burst: 10 pulses at 200 ms LOW.
- Expected: **all count** (200 ms > 50 ms threshold).

Result: 10/10 counted. Accumulator: 2802205 → 2802215.

### Test 3 — 50 ms threshold, well-below-threshold pulses

- Same build as Test 2.
- Burst: 10 pulses at 20 ms LOW.
- Expected: **none count** (20 ms < 50 ms threshold).

Result: 0/10 counted. Accumulator stayed at 2802215 through the entire
burst window; no `pulse(s) counted` log lines fired.

### Why not "threshold − 100 µs" per the ticket AC

The AC calls for injection at `threshold ± 100 µs`. That's below the
Pi's shell-driven timing resolution — a single `pinctrl set` call
takes several milliseconds (fork/exec/syscall). Tightening the AC
discriminator requires either:

1. A Pi-side helper that drives D7 via `mmap /dev/mem` (nanosecond-precise
   direct-write, no shell fork per edge).
2. A firmware-side `xiao-pulse.sh` replacement that has the XIAO drive
   D7 itself under program control — cleaner because the RTT clock
   already gives sub-microsecond timestamps.

Both are cheap follow-ups. The current 3× / 0.4× discrimination in Tests
2 and 3 already proves the filter accepts and rejects on the right side
of the threshold; it does not prove the exact threshold boundary matches
the Kconfig value. Landing the tightened discriminator (and moving it
into `tools/`) is scope for a separate PR.

## Filter did not observably wake the CPU

The RTT log shows no `pulse(s) counted` lines during the 20 ms burst
window in Test 3 — the sample loop's read of `hw_pulse_counter_read()`
returned the same value on each 200 ms tick, meaning TIMER2 didn't
increment. The rejection happened entirely on the PPI fabric.

No stronger evidence than "the counter didn't increment", though. A
proper wake-cost measurement requires the INA219 rig + a torch-flicker
noise source, which is deferred to when #35 is producing routine CSVs.

## Deferred (impl-2 and later)

- NVS-backed runtime override key + integration with the accumulator's
  `_valid` predicate (mirror #48's Divisor pattern).
- Manufacturer-specific Zigbee attribute so the threshold is
  field-adjustable via Z2M.
- External-converter surface (#59 AC clause + the pattern from #50).
- Pi-side sub-millisecond pulse driver (see the "why not ± 100 µs" note
  above) so the tightened AC discriminator can run.
- Host tests for the NVS round-trip (per the [[feedback_tdd_rule]]).

## Related

- Spike: `docs/working/2026-07-29-min-pulse-width-spike.md`.
- Ticket: #59.
- Memory pointer for the bench artifact: [[project_led_torch_flicker_pulse_count]] — the observation that motivated the ticket.

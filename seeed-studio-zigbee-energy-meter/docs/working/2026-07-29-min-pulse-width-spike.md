# Min-pulse-width filter — design spike (#59)

**Question the ticket asks**: can the min-pulse-width filter run entirely in
hardware on nRF52840 (Option A: LPCOMP → GPIOTE → TIMER capture chain via
plain PPI), or must we fall back to a CPU-wake gate in an ISR (Option B)?

**TL;DR**: Option A is feasible on nRF52840 plain PPI with 3 channels + 2
forks + one extra TIMER instance (TIMER3). The HFCLK-on window is naturally
bounded by `threshold_us × pulse_rate` — negligible in every plausible
scenario. Option B works for real meter LEDs but degrades precisely under
the torch-flicker / ambient-noise case the filter is designed to reject.

**Recommendation**: **Option A.** The ticket's first AC clause ("filter runs
entirely in hardware; CPU stays asleep during rejected crossings") is a
hard functional requirement, not a nice-to-have; Option B can't meet it
under the noise profiles that motivate the filter.

## Resource budget check

Current usage in the pulse chain (see `src/hw_pulse_counter.c`):
- **PPI**: 2 channels — LPCOMP UP → TIMER2 COUNT, GPIOTE (D7 bench) → TIMER2 COUNT.
- **TIMER**: TIMER2 in counter mode.
- **LPCOMP**: AIN0, VDD*3/8 ref, HYST on, UP-only detection (`enable-hyst` + `refsel = "VDD_3_8"` in `app.overlay`).

Available (nRF52840, `nrf52840_peripherals.h`):
- PPI: **20 configurable + 12 pre-programmed channels + 6 fork endpoints.** 802.15.4 driver + MPSL reserve some, but with 2 in use and plenty of headroom the budget is not the bottleneck.
- TIMER: TIMER0 (radio), TIMER1 (802.15.4), TIMER2 (this project's counter). **TIMER3 and TIMER4 are free** — DTS enables both nodes but `CONFIG_NRFX_TIMER3=n / CONFIG_NRFX_TIMER4=n` in the current build. TIMER3 is the natural pick.
- LPCOMP: EVENTS_UP, EVENTS_DOWN, EVENTS_CROSS all publish independently to PPI regardless of the ANADETECT mode (ANADETECT selects only the System-OFF wake source, not the PPI event lines). Confirmed against `hal/nrf_lpcomp.h`.

## Option A — pure hardware chain

### Wiring

```
LPCOMP AIN0 (phototransistor collector, VDD tap)
    │
    ├── EVENTS_UP  ── PPI ch A ──▶ TIMER3.TASKS_START
    │                    └─ fork ──▶ TIMER3.TASKS_CLEAR
    │
    ├── EVENTS_DOWN ─ PPI ch B ──▶ TIMER3.TASKS_STOP
    │                    └─ fork ──▶ TIMER3.TASKS_CLEAR
    │
    └── (nothing else)

TIMER3 (timer mode, 1 MHz, 16-bit)
    │  SHORTS: CC[0]_STOP  (auto-stop once threshold reached)
    │  CC[0] = threshold_us  (100 – 10 000)
    │
    └── EVENTS_COMPARE[0] ── PPI ch C ──▶ TIMER2.TASKS_COUNT
```

### Sequence of events

**Valid pulse (width ≥ threshold):**
1. Photodiode current rises → LPCOMP input crosses ref → EVENTS_UP fires.
2. PPI ch A: TIMER3 CLEAR + START. HFCLK auto-requested by TIMER3's PCLK16M dependency.
3. TIMER3 counts up. When CC[0] == threshold, EVENTS_COMPARE[0] fires.
4. CC[0]_STOP short freezes TIMER3. HFCLK request released (if nothing else needs it).
5. PPI ch C: TIMER2 COUNT — the pulse gets counted, same as today's chain.
6. Photodiode current falls → LPCOMP crosses down → EVENTS_DOWN fires.
7. PPI ch B: TIMER3 CLEAR + STOP (STOP is a no-op since already stopped; CLEAR arms for the next pulse).

**Rejected pulse (width < threshold):**
1. EVENTS_UP → CLEAR + START.
2. TIMER3 counting.
3. Before TIMER3 hits CC[0], EVENTS_DOWN fires.
4. PPI ch B: CLEAR + STOP. HFCLK released.
5. EVENTS_COMPARE[0] never fires → TIMER2 COUNT never triggers → **rejected**.

**CPU wake in either path: none.** All arithmetic and state transitions happen on the PPI fabric.

### HFCLK-on cost

TIMER3 in timer mode pulls PCLK16M, which auto-starts HFCLK. HFCLK is on
only while TIMER3 is enabled — from EVENTS_UP through either CC[0]_STOP or
EVENTS_DOWN, whichever comes first.

Per-pulse HFCLK-on window: `min(threshold_us, actual_pulse_width_us)`.
Bounded above by `threshold_us` (typical default: 1000 µs).

Worst-case duty under scenarios of interest:

| Scenario | Pulse rate | Per-pulse HFCLK-on | Duty | Average current impact |
|---|---|---|---|---|
| Real residential meter (1000 imp/kWh, 2 kW load) | 0.6 Hz | 1 ms (valid) | 0.06 % | ~1 µA |
| Real meter, 10 kW peak | 3 Hz | 1 ms | 0.3 % | ~5 µA |
| Torch flicker overcount (bench-observed) | ~30 Hz candidate crossings | ~100 µs each (rejected fast) | 0.3 % | ~5 µA |
| Pathological 100 Hz noise | 100 Hz | 1 ms max | 10 % | ~150 µA |

Anchor: HFCLK-on active current on this build measured as ~1.65 mA in
#57's SWD-attached run (see [[project_battery_current_hfclk_anchor]]);
`(active − sleep) × duty` gives the average impact. Even the pathological
100 Hz row stays inside the "well under 500 µA" bar #58 baseline projected
for practical battery life.

### PPI/timer channel accounting

- **3 PPI configurable channels** consumed for the filter path.
- **2 fork endpoints** consumed.
- **1 TIMER peripheral instance** (TIMER3).
- **0 GPIOTE channels** consumed (the filter path doesn't need any).

Post-filter chain totals: 5 PPI channels + 2 forks (out of 20 + 6) — plenty
of headroom for future work.

### Init-code shape (sketch)

```c
/* At init: */
nrfx_timer_init(TIMER3, {mode=TIMER, freq=1MHz, width=16b});
nrfx_timer_extended_compare(TIMER3, CC[0], threshold_us,
    TIMER_SHORTS_COMPARE0_STOP_MASK, /*enable_int=*/false);

alloc PPI ch A, B, C;

/* ch A: EVENTS_UP → START (+ fork CLEAR) */
gppi_channel_endpoints_setup(A,
    nrf_lpcomp_event_address_get(LPCOMP, EVENT_UP),
    nrfx_timer_task_address_get(TIMER3, TASK_START));
gppi_fork_endpoint_setup(A,
    nrfx_timer_task_address_get(TIMER3, TASK_CLEAR));

/* ch B: EVENTS_DOWN → STOP (+ fork CLEAR) */
gppi_channel_endpoints_setup(B,
    nrf_lpcomp_event_address_get(LPCOMP, EVENT_DOWN),
    nrfx_timer_task_address_get(TIMER3, TASK_STOP));
gppi_fork_endpoint_setup(B,
    nrfx_timer_task_address_get(TIMER3, TASK_CLEAR));

/* ch C: TIMER3 CC[0] event → TIMER2 COUNT */
gppi_channel_endpoints_setup(C,
    nrfx_timer_compare_event_address_get(TIMER3, 0),
    nrfx_timer_task_address_get(TIMER2, TASK_COUNT));

gppi_channels_enable(BIT(A) | BIT(B) | BIT(C));

/* LPCOMP: switch detection from UP-only to CROSS so DOWN events fire. */
nrfx_lpcomp_config_t cfg = NRFX_LPCOMP_DEFAULT_CONFIG(NRF_LPCOMP_INPUT_0);
cfg.detection = NRF_LPCOMP_DETECT_CROSS;  /* was UP */
cfg.reference = NRF_LPCOMP_REF_SUPPLY_3_8;
cfg.hyst = NRF_LPCOMP_HYST_ENABLED;
```

Threshold retune at runtime (via the future Zigbee attribute write):

```c
void hw_pulse_counter_set_min_width_us(uint32_t us)
{
    /* CC[0] can be written live — SHORTS logic just uses the current
     * value. Cheap: single register write, no re-init.
     */
    nrfx_timer_extended_compare(TIMER3, CC[0], us,
        TIMER_SHORTS_COMPARE0_STOP_MASK, /*enable_int=*/false);
}
```

## Option B — GPIOTE-on-LPCOMP + software gate

Enable LPCOMP UP + DOWN IRQs. ISR reads TIMER3 capture on UP (start),
captures on DOWN (stop), computes delta. If delta ≥ threshold: call
`nrf_timer_task_trigger(TIMER2, TASK_COUNT)`.

CPU wake cost per candidate crossing pair (UP + DOWN): ~50 µs each × 2
wakes = ~100 µs from WFE to WFE, at ~3 mA active vs. sleep floor.

| Scenario | Pulse rate | Wake duty | Average current impact |
|---|---|---|---|
| Real meter, 3 Hz | 6 wakes/s | 0.03 % | ~1 µA |
| Torch flicker (bench) | 60 wakes/s | 0.3 % | ~9 µA |
| Pathological 100 Hz noise | 200 wakes/s | 1 % | ~30 µA |
| Sunlight glinting off meter housing (imagine) | 500 wakes/s | 2.5 % | ~75 µA |

The failure mode: Option B's average current rises **with the noise rate**.
The filter's whole reason to exist is to protect the count against
inflated crossings from ambient flicker; Option B trades that count-side
protection for a proportional sleep-current penalty. Exactly backwards from
what the ticket wants.

## Head-to-head

| | Option A | Option B |
|---|---|---|
| Meets AC "filter runs entirely in hardware; CPU asleep during rejected crossings" | **Yes** | No |
| Init code complexity | Higher (3 PPI channels + forks + TIMER3 + SHORTS + LPCOMP detect change) | Lower (LPCOMP IRQ + delta compare) |
| Extra peripheral resource | TIMER3 (previously unused) | TIMER3 capture, but timer runs continuously so HFCLK on always — worse than A |
| PPI channel cost | 3 + 2 forks (still 15 configurable free) | 0 |
| Sleep-current cost — real meter | ~1–5 µA | ~1–9 µA |
| Sleep-current cost — noisy environment | Bounded by `threshold × rate`; ≤150 µA worst case | Grows with wake rate; can exceed 75 µA in credible scenarios |
| Runtime retune of threshold | Single-register write | Variable assignment |
| Bench verification | Same for both: `tools/xiao-pulse-burst.sh` at threshold ± 100 µs |

## Non-goals (still non-goals after the spike)

- **Min-gap / debounce** (post-pulse hold-off). Different noise profile, out
  of scope; the ticket explicitly parked it as a separately-trackable
  follow-up if bench evidence warrants.
- Backfilling the D7 bench pulse chain through the same filter. Currently
  D7 GPIOTE → TIMER2 COUNT directly (bypassing LPCOMP entirely). Bench
  injects are meant to be 1:1 test signals; passing them through the filter
  would make bench verification harder, not easier. Leave the D7 path
  filter-bypassed.

## Follow-up implementation scope (what a PR looks like)

1. `CONFIG_APP_PULSE_MIN_WIDTH_US` (int, default 1000, range 100–10000) with rationale comment.
2. NVS-backed runtime override — new key in `src/nvs_store.c`, same shape as the divisor override from #48.
3. Manufacturer-specific Zigbee attribute (0x0702 metering cluster has no natively suitable attribute — need a manufacturer-specific one; see #59 body).
4. `hw_pulse_counter_set_min_width_us(uint32_t)` public entry point.
5. Init-code changes per the Option A sketch above; keep the D7 GPIOTE → TIMER2 direct path untouched.
6. Update `app.overlay` to enable TIMER3.
7. Host-test coverage: threshold validation + NVS round-trip (matches #48's testing pattern; see [[feedback_tdd_rule]]).
8. Bench verification per the ticket's AC — `tools/xiao-pulse-burst.sh` at threshold ± 100 µs → expect 0 vs 1000 counts respectively.
9. External-converter surface (#59 AC item, follow-up).

Related memory: [[project_led_torch_flicker_pulse_count]] (bench evidence
that motivated the ticket), [[reference_z2m_write_read_only_attr_bypass]]
(pattern for the manufacturer-specific attribute path).

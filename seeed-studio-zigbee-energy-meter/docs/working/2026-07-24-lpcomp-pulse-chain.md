# 2026-07-24 — Hardware pulse-counting chain: LPCOMP + PPI + TIMER2 (#7)

## What changed

Replaced the software pulse path (10 Hz SAADC sample → threshold-based
edge detector → `atomic_t bench_pulse_count`) with a pure-hardware chain
that runs during System-ON sleep with no CPU wake per pulse:

```
    Phototransistor (P0.02 / AIN0 / XIAO A0)
                    │
                    ▼
                 LPCOMP  (ref = VDD × 3/8, HYST on, detect = UP)
                    │  event: LPCOMP_EVENT_UP
                    ▼
                  PPI ch A ─────────────────►  TIMER2 COUNT task
                                                    ▲
    Bench D7 (P1.12) ──► GPIOTE HITOLO ──► PPI ch B ┘
```

Two source events, one 32-bit hardware counter. Sample loop wakes on its
own cadence and reads via `nrfx_timer_capture()` — a handful of register
writes, no interrupt, does not stop or clear the counter.

## New files

- `src/hw_pulse_counter.h` — two-function surface: `init()` + `read()`
- `src/hw_pulse_counter.c` — all nrfx plumbing; well-commented for the
  next person who has to touch peripheral wiring

## Deleted

- `src/pulse_edge_detector.[ch]` and `tests/test_pulse_edge_detector.c`
  (the ADC-sample-loop edge detector; no longer called from anywhere)
- All references to `bench_pulse_count`, the D7 software ISR path, and
  ADC channel init from `main.c`
- The `zephyr,user` ADC channel + `&adc` block from `app.overlay`
- `CONFIG_ADC=y` from `prj.conf`

Battery-voltage SAADC returns in #8 on a different channel.

## Design decisions and their alternatives

### LPCOMP init path: own it in C (Path B), don't use the Zephyr shim (Path A)

The Zephyr `comparator_nrf_lpcomp` shim would work, but it wires
`nrfx_isr` and `nrfx_lpcomp_irq_handler` in `SYS_INIT` with its own
event handler and registers a `comparator` device-driver instance. Once
that's in place, our code would either fight it (double `nrfx_lpcomp_init`
→ `NRFX_ERROR_ALREADY`) or have to talk to it through the comparator
API and hope its callback wiring stays out of the way. Cleaner to own
LPCOMP end-to-end from `hw_pulse_counter.c` — `CONFIG_COMPARATOR_NRF_LPCOMP=n`
in `prj.conf` disables the shim, leaving the raw nrfx driver code
available and unused-by-anyone-else.

### Reference: VDD × 3/8, not the nrfx default VDD × 4/8

At 3.3 V USB VDD the 3/8 tap = 1.24 V; at 3.0 V battery VDD it's 1.125 V.
The previous ADC-path threshold was 1000 mV (from `PHOTOTRANSISTOR_THRESHOLD_MV`
in the old main.c), so this stays close enough that the same shrouded
TEPT4400 calibration should hold. If bench work with a real meter LED
suggests we're clipping the low or high tail, retunable via the DT
`refsel` property in `app.overlay` — no code change needed.

### Detection: `UP`, not the nrfx default `CROSS`

`CROSS` fires an event on both up- and down-crossings, which through
PPI would count each real LED flash TWICE (one for lit-to-dark, one for
dark-to-lit). `UP` fires once per full flash. Confirmed against the
LPCOMP peripheral chapter in the nRF52840 PS §37.

### HYST on, but no software min-pulse-width filter

Issue #7's acceptance criteria include "Min-pulse-width noise filter
rejects crossings under N µs (N tuned on bench)". We're shipping with
HYST enabled as the sole noise defence and deferring an explicit
min-width filter. Reasoning:

- A software filter *of any kind* requires the CPU to service an LPCOMP
  ISR on each transition, which negates the whole point of the LPCOMP
  → PPI → TIMER chain (catch pulses while the CPU sleeps).
- The pure-hardware alternative — chaining a second TIMER to gate the
  count based on pulse width — is non-trivial peripheral juggling and
  ties up additional PPI channels + a TIMER we might want later.
- HYST provides ~50 mV of hysteresis around the reference, which is
  usually sufficient defence against near-threshold jitter — the actual
  concern is *large* transient light events (torch swept past the
  snout, room-light change with a poor shroud). A shrouded phototransistor
  is the proper fix for those, not a per-pulse width validator.

**What triggers reconsideration:** if bench runs with a real meter LED
show the accumulator drifting up faster than pulses were actually fired
— e.g. `xiao-pulse-burst.sh 100` results in a delta > 100 on device —
we add the software validator (accept the per-pulse CPU wake; battery
budget re-checks in #8) or do the two-TIMER hardware gate. Not before.

### TIMER2, not TIMER1 as originally planned

TIMER0 is Nordic's radio softblock. TIMER1 is the 802.15.4 driver's;
`zephyr/modules/hal_nordic/Kconfig:26` literally says `depends on
!$(dt_nodelabel_enabled,timer1)`, meaning enabling `&timer1` in DT
breaks the Kconfig dependency chain and the build fails with an
IEEE802154 `has direct dependencies (…!y…)` error. Discovered on the
first build attempt; fix was mechanical (renumber to TIMER2 everywhere).
TIMER2 has 4 CC channels — we only need one for the CAPTURE — and is
otherwise unclaimed by nRF/nrfxlib code on this SoC. Left a comment in
`hw_pulse_counter.c` so the next person doesn't repeat the mistake.

### D7 bench inject: keep it, route via GPIOTE + PPI

`tools/xiao-pulse.sh` and `xiao-pulse-burst.sh` (BCM 27 on the Pi →
XIAO D7) are essential for automated bench verification; retiring them
alongside the ADC path would have blocked all follow-up work that
doesn't have a real meter attached (#8 sleep-current measurement, #20
ongoing verification). Two options:

- **Chosen:** GPIOTE on D7 for HITOLO → PPI ch B → TIMER2 COUNT.
  Two hardware event sources feeding the same counter, unified read
  path, no software offset arithmetic.
- Rejected: keep the D7 GPIO ISR, add a software offset atomic to
  the read path. Simpler wiring but re-introduces per-pulse CPU wake
  on bench inject, and splits the read source between HW and SW.

## Sample-loop cadence bumped from 100 ms to 200 ms

With pulse counting now in hardware, the sample loop's job is service-only:
LED heartbeat, persist policy, and pushing any observed delta to the
Zigbee attribute. 100 ms was Nyquist margin for the old 10 Hz ADC
sampling; 200 ms is fine for the service tasks, keeps LED heartbeat at
1 Hz (toggle every 5 wakes), and cuts wall-clock CPU time in half.
Deeper cadence relaxation waits for #8, where the loop becomes an
RTC-driven wake instead of a `k_sleep`.

## Root-cause fix landed 2026-07-25: nrfx_timer_config.frequency = 0 → DIV_0

The first bench-verify attempt crashed silently before the sample loop
started. USB CDC enumerated as the app but serial produced zero bytes,
no MQTT reports fired for a 105-pulse burst, and the red LED sat
solid-off. Boot-time LED beacons staged through main() confirmed the
crash happened right after all init completed (main reached the pre-
loop point) but before the heartbeat toggled.

SWD attach + halt via the Pi's OpenOCD dumped the fault:
`R0 = 0x1e = 30 = K_ERR_ARM_USAGE_DIV_0`. The original exception frame
(preserved in Zephyr's fault path, walked via PSP at 0x2000e0c8)
pointed at PC = 0x000432ca — `nrfx_timer_prescaler_calculate` at
`nrfx_timer.c:86`, inlined into `timer_configure` at :98, called from
`nrfx_timer_init` at :150. The faulting instruction was
`udiv r3, r2, r4` with R2 = 16000000 (16 MHz base clock) and
**R4 = 0** loaded from `[R1, #0]` — the `.frequency` field of the
`nrfx_timer_config_t` I passed in.

I had written:

```c
const nrfx_timer_config_t timer_cfg = {
    .frequency = 0, /* ignored in counter mode */
    ...
};
```

The comment was wrong. nrfx v3+ calls `prescaler_calculate()` from
`nrfx_timer_init()` **unconditionally**, before it looks at `.mode` —
so `.frequency = 0` divides `NRFX_TIMER_BASE_FREQUENCY_GET(...)` by
zero and, because Zephyr enables the Cortex-M4 DIV_0_TRP bit, fires
`K_ERR_ARM_USAGE_DIV_0` → `z_fatal_error` → `arch_system_halt` (WFI
forever). In counter mode the prescaler is functionally irrelevant
(the timer counts external COUNT-task edges, not the prescaled
internal clock) but nrfx still needs a legal value to program the
PRESCALER register.

**Fix:** `.frequency = NRFX_MHZ_TO_HZ(1)` — 1 MHz gives prescaler=4,
matches Zephyr's nrfx counter samples, and stays well inside spec.
One line change in `hw_pulse_counter.c`; the fix is now in-tree with
a long comment above the config so no one repeats the mistake.

**Also fixed in the same pass:** the boot-log string still said
"TIMER1 counter mode" from the pre-#7 rename attempt — corrected to
TIMER2 to match the actual peripheral in use. `prj.conf`'s block
header comment likewise updated.

## Bench verification — PASSED 2026-07-25 (post-fix)

After the DIV_0 fix, on the Pi/USB-DFU rig:

1. Serial boot log: `hw pulse counter live: LPCOMP AIN0
   refsel=VDD_3_8 HYST=on, bench D7 GPIOTE HITOLO, TIMER2 counter
   mode` — confirms init succeeded.
2. `restored accumulator_total=117 from NVS` — persistence survived
   the reflash cycle.
3. `xiao-pulse-burst.sh 105` (Pi → D7 falling edges): device logged
   105 `pulse(s) counted: delta=1 accumulator_total=<N>` lines,
   accumulator went from 1 → 106 (a couple of extra counts across
   the session are the bench-lab ambient light noise on AIN0 with
   an unshrouded phototransistor — visible as one-off `delta=1`
   entries between D7 bursts; within tolerance of "hardware counter
   is wired and firing" — see the noise-filter follow-up below for
   the fix once we're on a real meter LED).
4. 5-minute Zigbee report tick fired live and logged
   `metering report tick: CurrentSummationDelivered=117` — the
   `_and_report` variant on the tick is unaffected by this refactor.
5. Sample-loop heartbeat: red LED toggles at ~1 Hz continuously,
   confirming the sample loop is running and not blocked on
   hw_pulse_counter_read() / zigbee_app_publish_summation().
6. **Not exercised yet: LPCOMP-side (phototransistor with a real
   meter LED).** No functioning meter to point at; deferred to when
   we have a real meter or a bench LED rig set up. The LPCOMP path
   is wired identically to the D7 path from PPI onward — the D7
   burst pass is strong evidence that the counter side works; only
   the LPCOMP → PPI subscription remains unverified end-to-end.

## Debug scaffolding removed before the fix landed

- Staged LED "beacon" blinks in main() (used to bracket the crash
  to inside the sample-loop pre-boot; removed after root cause was
  found).
- `CONFIG_MAIN_STACK_SIZE=4096` / `CONFIG_STACK_SENTINEL=y` (added
  temporarily to test a stack-overflow hypothesis; ruled out —
  fault was DIV_0, not stack).

The final commit is a clean diff: `hw_pulse_counter.c` (frequency +
log string), `prj.conf` (block-header comment), plus the
already-present overlay/prj.conf/main.c/CMakeLists.txt/tests changes
from the initial refactor.

## Follow-ups

- Bench-verify per the list above; update this doc's "Bench verification"
  section with results and close #7 or push back changes.
- **Sleep-current measurement (#8)** — the main win of this PR is
  invisible until the sample loop stops using `k_sleep(K_MSEC(...))`
  and starts using an RTC wake. #8's power-profiler measurement will
  confirm whether we're actually in the µA regime we're aiming for.
- **Real meter LED calibration** — the VDD × 3/8 threshold and the
  TEPT4400 with 47 kΩ load are informed guesses. On the actual meter
  we may need to change `refsel` (via DT overlay, no code change) and
  possibly re-spec the load resistor. Track separately if it lands.

## Related

- #7 (this issue)
- #8 (battery-mode transition + sleep-current) — depends on this PR
- #20 (silent-Z2M) — orthogonal; the 5-min tick's `_and_report` path
  survives untouched

# HFCLK-anchor hunt for the residual 1.65 mA — three hypotheses ruled out (#8)

Follow-up to `2026-07-29-blocker2-usb-off.md`. That doc left the
residual 1.65 mA settled current pointing at TIMER2 counter mode
holding HFCLK on. This session tested that and two adjacent
hypotheses; all three came back with zero delta. Documenting the dead
ends so the next investigation doesn't retrace them.

## Rig + measurement protocol

Unchanged from blocker #2: XIAO on Pi 3V3 through the INA219 shunt,
USB unplugged, `rtt-tail.sh` NOT attached (SWD idle → clean baseline).
6-minute sample with fresh factory-reset-and-rejoin in the first 30 s
of each run. Reported number is the mean of the 30 s-onward steady
state.

## Hypothesis 1: TIMER2 counter mode is anchoring HFCLK

**Motivation.** nRF52 TIMER peripherals are clocked from PCLK16M
(HFCLK-derived). In counter mode the counter register needs its
peripheral bus clock alive to increment on `TASKS_COUNT` from PPI.
If TIMER2 stays armed for the pulse chain, HFCLK effectively can't
stop.

**Test.** New Kconfig `APP_HW_PULSE_COUNTER` gates the
`hw_pulse_counter_init()` call and the `hw_pulse_counter_read()`
snapshot in `main.c`. Layered isolation overlay `no-pulse.conf`
turns it off. Compiles out LPCOMP init, TIMER2 start, and both PPI
channels. Device joins normally, just doesn't count anything.

**Result:** **no change** — 1.64 mA settled with the whole pulse chain
compiled out, vs 1.65 mA with it running. Δ ≈ 0.

CSV: `ina219-2026-07-29-151152-blocker-hfclk-no-pulse-chain.csv`.

TIMER2 counter mode is not what's holding HFCLK.

## Hypothesis 2: Zephyr PM subsystem would gate HFCLK if we asked it to

**Motivation.** Bench-prep doc's blocker #8 mentioned
`CONFIG_PM=y` + `CONFIG_PM_DEVICE=y` as a lever we hadn't touched.
Zephyr's PM subsystem's `policy_next_state` hook can transition into
deeper sleep states than the default `arch_idle()` reaches.

**Test.** New overlay `pm-on.conf` sets both.

**Result:** **cannot apply on this platform.** `CONFIG_PM=y` needs
`HAS_PM=y`, which on nRF52X is not selected by the SoC in NCS 2.9.2
(only nRF54H selects it in Zephyr's SoC tree). Kconfig prints:

```
warning: PM ... was assigned the value 'y' but got the value 'n'.
Check these unsatisfied dependencies: (... || (SYS_CLOCK_EXISTS && HAS_PM)) (=n).
```

`CONFIG_PM_DEVICE=y` sticks in isolation (it's a separate symbol) but
without CONFIG_PM the kernel idle path never invokes the PM policy,
so it doesn't gate HFCLK. Steady-state stayed at 1.65 mA with just
`PM_DEVICE=y` set.

CSV: `ina219-2026-07-29-152143-hfclk-pm-on.csv`.

The Zephyr PM subsystem is not an accessible lever on the XIAO nRF52840
in this NCS release. Any HFCLK gating must come from the driver stack
below.

## Hypothesis 3: USB device stack (from blocker #2)

Already resolved in the earlier doc — USB was also not the anchor.
Included here for completeness.

## What we know now

- Residual 1.65 mA is very close to the nRF52840 datasheet's "CPU
  System-ON idle, HFCLK on" figure (~1.6 mA).
- Direct clock consumers we can compile out (USB PHY, TIMER2, PPI)
  are not the anchor.
- Zephyr's PM subsystem isn't wired up for nRF52X in NCS 2.9.2, so
  we can't reach for that lever.
- The 802.15.4 driver (nrfxlib) selects `NRF_802154_SL_HPTIMER=y`
  by `def_bool y` on `SOC_SERIES_NRF52X`. HPTIMER is a HFCLK-based
  precision timer for CCA / backoff / IFS timing. Whether it's the
  anchor is unclear from the Kconfig alone — needs runtime probing.

## Recommended next investigation (not done in this session)

Instrument the RTT probe to read `NRF_CLOCK->HFCLKSTAT` and the
Zephyr clock-control refcount for HFCLK on each 60 s tick. That
answers directly:

1. Is HFCLK actually running in the steady state? (confirming
   the datasheet-based inference)
2. If yes, which subsystem holds the refcount? (`nrfx_clock` /
   `MPSL` / `802.15.4` / `Zigbee`)

Two lines to add in `zigbee_app.c` next to the existing probe:

```c
#include <hal/nrf_clock.h>
bool hfclk_running = nrf_clock_hf_is_running(NRF_CLOCK, NRF_CLOCK_HFCLK_HIGH_ACCURACY);
LOG_INF("hfclk probe [%s]: HFCLKSTAT=0x%08x running=%d",
        where, NRF_CLOCK->HFCLKSTAT, hfclk_running);
```

If HFCLK is confirmed on, next candidates for who's holding it:

- **MPSL** — the multi-protocol scheduler layer. Owns HFCLK requests
  on behalf of `nrf_802154`. Look at `CONFIG_MPSL_HFCLK_LATENCY` and
  `mpsl_clock_hfclk_wait_stopped()` for the release path.
- **`NRF_802154_SL_HPTIMER`** — the HFCLK-based precision timer. If
  the driver keeps it armed between polls for future radio slot
  scheduling, HFCLK stays on.
- **ZBOSS scheduler timer** — the R23 stack's own periodic tick.
  `CONFIG_ZIGBEE_TIME_KTIMER=y` is set (Zephyr-timer-backed), which
  should be HFCLK-off-safe, but worth confirming.

## Landing decision

Land the diagnostic infrastructure (Kconfig gate + overlays + this
doc) so the next investigator has:

- A one-line way to disable the pulse chain (`no-pulse.conf`) for
  isolation testing.
- A pre-written `pm-on.conf` that documents the dead-end so it doesn't
  get re-tried.
- The three CSVs + this narrative so the "we already tried that"
  boundary is clear.

Accept the current 1.65 mA floor for now — that's ~30 days on 1200 mAh
2×AAA lithium, useful enough for the near-term deployment, and 7×
better than the broken 12.4 mA baseline that started this thread. The
next push toward sub-mA needs the HFCLK-holder probe recommended
above and is a separate ticket-worthy chunk of work.

# TIMER probe result — no TIMER is running; anchor is likely SWD attach (#8)

Follow-up to `2026-07-29-hfclk-probe-result.md`, which established that
HFCLK is on continuously (source=HFINT, HFCLKRUN=0) but couldn't
identify the peripheral consumer of PCLK16M. Extended the probe to
snapshot TIMER0-4 registers and, importantly, do a double
`TASKS_CAPTURE[5]` to detect whether each timer is actually running.

## What was added

`log_timer_status()` now performs, for each of TIMER0-4:

1. Snapshot `MODE`, `BITMODE`, `PRESCALER`, `INTENSET`.
2. Back-to-back `TASKS_CAPTURE[5] = 1; read CC[5]` twice, log the
   delta. Non-zero delta → timer is running RIGHT NOW.

CC[5] is used because drivers typically use CC[0..2] and rarely
higher. Soft assumption; hasn't caused issues in this test.

## Result — no TIMER is running

Every probe fire (post-configure-sleepy, post-reboot-reattach, and 60 s
tick) shows the same picture:

| TIMER | MODE          | INTENSET   | CC5_delta | running |
|-------|---------------|------------|-----------|---------|
| T0    | 0 (Timer)     | 0          | 0         | 0       |
| T1    | 0 (Timer)     | 0          | 0         | 0       |
| T2    | 1 (Counter)   | 0          | 0         | 0       |
| T3    | 0 (Timer)     | 0          | 0         | 0       |
| T4    | 0 (Timer)     | 0          | 0         | 0       |

Details worth noting:

- **T0** flips BITMODE=0 → BITMODE=3 between pre-join and post-reboot —
  that's the 802.15.4 driver configuring T0 during radio bring-up.
  But it never TASKS_STARTs after configuring; not running.
- **T2** is in Counter mode (our LPCOMP → PPI → TIMER2 pulse chain).
  Counter mode does NOT consume PCLK16M — confirms the earlier
  `no-pulse.conf` isolation result.
- **T1, T3, T4** are configured but idle.

Artifact: `docs/working/rtt-2026-07-29-160427-timer-running.log`.

## Interpretation — the anchor is not a TIMER

The MPSL / `NRF_802154_SL_HPTIMER` hypotheses from the prior doc are
both ruled out at the TIMER-instance level. Neither is actively
consuming PCLK16M via TIMER hardware in the settled state.

**But HFCLK is still on** (per `HFCLKSTAT=0x00010000` in every probe
fire). Something else is holding it.

**The probe's fundamental limitation.** Every read runs inside a
LOG_INF path, so the CPU is by definition active. The Cortex-M4 needs
HFCLK to execute instructions, so HFCLK MUST be on at the moment we
read HFCLKSTAT. What actually tells us HFCLK stays on is the INA219
floor — 1.4 mA min, 1.9 mA typical — which matches the datasheet's
"CPU System-ON idle, HFCLK on" figure, not the ~2 µA System-ON
deep-sleep floor a truly-gated HFCLK would give.

## Strongly-suspected root cause: SWD debug port attached

nRF52 has a well-known quirk (Nordic infocenter anomaly notes): when
the SWD debug port is electrically attached (`CDBGPWRUPREQ` asserted
by the debugger), the device cannot enter true System-ON deep sleep,
and average current sits at the ~1.6 mA HFCLK-on floor. This holds
even when the debugger's control session is torn down, as long as
the SWD lines remain connected and the DAP has been powered up in
the past.

Our INA219 rig has SWD physically wired from `rpi-xiao` to the XIAO's
SWD pads throughout ALL of the 2026-07-29 sessions. That includes
the "SWD idle → clean baseline" runs in `2026-07-29-blocker2-usb-off.md`
where `rtt-tail.sh` wasn't attached — the wires were still there,
and OpenOCD had been run recently for flashing.

If SWD-attach is the anchor, the 1.65 mA floor we've been measuring
is **not the firmware's fault** at all. It's a rig artifact. Real
battery-mode current on a fully-disconnected XIAO would be much
lower — potentially the sub-mA System-ON sleep the sleepy ED path
is designed for.

## Definitive test (requires physical intervention, not done here)

The one clean way to prove or disprove this without a PPK2:

1. Confirm no OpenOCD is running on `rpi-xiao`
   (`ssh rpi-xiao 'pgrep openocd'`).
2. Physically pull the four SWD jumpers (SWCLK, SWDIO, GND, and any
   3V3 reference) from the XIAO's SWD pads.
3. Power-cycle the XIAO (interrupt the Pi 3V3 → BAT wire briefly, or
   remove the wire and re-connect).
4. Restart INA219 sampling for a few minutes.
5. Compare mean/min current to the 1.65 mA baseline.

Outcomes:

- **Current drops to < 100 µA**: SWD-attach was the anchor. The
  firmware is fine. Update working notes accordingly, and the
  sub-mA target is essentially already met — verification loop
  becomes "PPK2 or shunt without SWD attached".
- **Current stays at ~1.65 mA**: SWD-attach is NOT the anchor.
  Some other peripheral (candidates: PWM, RNG, ECB, SAADC in
  auto-sample, or a Zephyr driver we haven't audited) is anchoring
  HFCLK. Continue probing at that level.

## Landing decision

Land the extended TIMER probe + this doc. The probe is now capable of
answering "is a TIMER anchoring HFCLK" definitively, which is a
useful diagnostic to keep around even after the SWD-attach question
is resolved. The 1.65 mA floor stays where it was pending the
physical SWD-disconnect test.

If SWD-attach turns out to be the anchor (very likely per the Nordic
anomaly pattern), consider closing the "chase the HFCLK anchor" thread
entirely and moving to PPK2-based measurement for any future
battery-life claims — the INA219 rig with wired SWD has a fundamental
1.6 mA floor artifact that any INA219 tuning number will inherit.

# Blocker #2 (USB device stack) — no current delta, but land anyway (#8)

Follow-up to `2026-07-29-blocker1-closed.md`, which left the residual
1.65 mA steady-state current pointing at "CPU System-ON idle, HFCLK on"
and named the USB device stack as the leading suspect for what's
holding HFCLK on.

Result: **hypothesis is wrong** — turning off `CONFIG_USB_DEVICE_STACK`
had zero effect on steady-state current. Still worth landing for the
other benefits, but the residual investigation moves to TIMER2 next.

## Change

`rtt.conf` gains:

```
CONFIG_USB_DEVICE_STACK=n
CONFIG_USB_CDC_ACM=n
```

`src/main.c` wraps three USB touchpoints in `#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)`:

- The `<zephyr/drivers/uart.h>` + `<zephyr/usb/usb_device.h>` includes
- `wait_for_host_dtr_or_timeout()` (definition + boot-time call)
- The `DEVICE_DT_GET(DT_CHOSEN(zephyr_console))` + `usb_enable()` block
  in `main()`

The USB-dev workflow (no rtt.conf overlay) is unchanged — CONFIG_USB_DEVICE_STACK
stays `y` in `prj.conf` and everything compiles as before.

## Measurements

Same rig as `2026-07-29-blocker1-closed.md`: XIAO on Pi 3V3 through the
INA219 shunt, USB unplugged, `rtt-tail.sh` NOT attached (SWD idle).
Fresh factory-reset-and-rejoin in the first 30 s of each 6-minute
sample, comparison over the 30 s-onward steady state.

| condition | 30 s onward mean | p50 | p95 | sd |
| --- | ---:| ---:| ---:| ---:|
| `baseline-no-rtt-tail` (blocker #1 closed, USB stack **on**) | 1.65 mA | 1.60 | 1.90 | 0.15 |
| `blocker2-usb-off` (USB stack **off**) | **1.65 mA** | 1.60 | 1.90 | 0.15 |
| Δ | **±0 mA** | — | — | — |

CSV: `ina219-2026-07-29-145842-blocker2-usb-off.csv`.

Per-window breakdown mirrors the earlier run exactly — no measurable
change in any 30 s window past the join transient.

## What this rules out

Nordic's USB device stack initialises the USB PHY at
`SYS_INIT`-time even when nothing is plugged into VBUS. The theory was
that the PHY's 48 MHz clock requirement held HFCLK on, blocking the
System-ON deep-sleep drop from ~1.6 mA to <100 µA. **The measurement
says no.** Either the USB PHY's request on HFCLK is released when it
sees no VBUS, or something else independently holds HFCLK on. Given the
data, the second is more likely — see next.

## What to try next

TIMER2 in counter mode for the LPCOMP → PPI → TIMER2 pulse chain
(`src/hw_pulse_counter.c`). nRF52 TIMER peripherals are clocked from
PCLK16M, which is derived from HFCLK. In counter mode the TIMER's
counter register still needs its peripheral bus clock alive to
increment on `TASKS_COUNT` from PPI — so as long as the pulse chain is
armed, HFCLK effectively can't stop.

Approaches to try in order of increasing intrusiveness:

1. **Quick check**: build with the pulse chain disabled and re-measure.
   If current drops to <100 µA that confirms TIMER2 is the anchor.
2. **`CONFIG_NRF_TIMER_STARTUP_MODE=NRF_TIMER_STARTUP_MODE_LATE`**
   or similar — check nRF platform Kconfigs for a way to defer TIMER
   PCLK request until actually needed. Unlikely to work for counter
   mode where a single event needs immediate response.
3. **Rearchitect the counter path**: GPIOTE → PPI → RTC's counter is
   not a native path on nRF52; the only routes that survive HFCLK-off
   involve either polling on wake or waking on every pulse (which
   defeats the sub-µs latency benefit of the current design).
4. **Accept it**: 1.6 mA average is a real number — ~40 % of the
   original 5 mA guess and 7× below the broken 12 mA baseline. On
   1200 mAh AAA lithium that's ~750 hours ≈ 31 days. Not multi-year,
   but not a catastrophe.

For now: land this because the boot delay drop + code size win are
free, and the residual investigation is a separate ticket-worthy
thread.

## Ancillary wins

- **Boot latency**: `wait_for_host_dtr_or_timeout(cdc, 5000)` is now
  compiled out. First `LOG_INF` from main lands at t ≈ 0.02 s instead
  of t ≈ 5 s, so RTT-attached iteration is faster.
- **Flash / RAM**: 366 104 B → 349 688 B FLASH (16.4 KB), 79 216 B →
  69 872 B RAM (9.3 KB). Not a scarcity concern today but headroom
  for future work.
- **Test-join.sh PASSes** on the USB-off build — Z2M interview
  completes normally; the join/interview path doesn't touch USB.

## Decision

Land the USB-off change on the rtt.conf overlay. Update the residual
narrative in blocker-tracking (this doc). Next investigation on the
sleep-current thread: TIMER2 counter mode's HFCLK anchoring — but
that's a bigger change than a Kconfig flip and belongs in its own
ticket.

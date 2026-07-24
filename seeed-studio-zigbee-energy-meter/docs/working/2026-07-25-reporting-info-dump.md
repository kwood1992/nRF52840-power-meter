# 2026-07-25 — Diagnostic dump of the reporting slot (#20)

## What changed

`send_explicit_summation_report()` in `src/zigbee_app.c` gains one
`LOG_INF` line, printed on the ZBOSS thread immediately after the
`zb_zcl_find_reporting_info()` NULL check succeeds (i.e. before the
OUT-buffer allocation and the actual send).

The log format:

```
reporting slot: min_interval=<N>s max_interval=<N>s reportable_change=0x<hi><lo>
```

Fields come straight from `zb_zcl_reporting_info_t.u.send_info`:

- `min_interval` — u16 seconds; expected `0` per #17.
- `max_interval` — u16 seconds; expected `65000` per current attrib list.
- `reportable_change` — the u48 delta packed as `0x` + 4 hex digits (high
  u16) + 8 hex digits (low u32). Design-doc default = 100 =
  `0x000000000064`.

## Why (per #20)

Suspected cause #1 in the issue is that after repeated factory-resets
the coordinator's `configured_reportings` view drifts out of sync with
the ZBOSS-side reporting slot on the device. This line proves — from
the device side, on the actual ZBOSS thread that will send the frame —
that the slot survived the join and that the delta / interval numbers
match what Z2M's `bridge/devices` claims. If the numbers differ, the
device and coordinator disagree; if the slot is missing entirely, the
pre-existing `LOG_WRN "no reporting slot"` still fires and this line
does not print.

Cadence is inherently rate-limited: this only runs inside the explicit-
send path, which is already gated by the 100-pulse heartbeat and the
5-minute periodic tick (see `2026-07-24-explicit-report-fallback.md`).
No new Kconfig knob.

## How to read it

Serial: `cu -l /dev/cu.usbmodem<n> -s 115200` (NOT `tty.usbmodem*` —
see memory `project_macos_cu_vs_tty_usbmodem.md`; `tty.*` blocks on
DCD which CDC-ACM doesn't assert). Grep for `reporting slot:`.

Expected first appearance is on the 100th pulse-driven publish since
boot, or on the first 5-min tick after the device has joined and Z2M
has run ConfigureReporting — whichever comes first.

# Zigbee join as sleepy end device (issue #4)

## What was added

- `button_press_classifier.[ch]` — pure-logic short/long-press classifier
  with a deliberate NEITHER band between 1 s and 3 s. Host-tested
  (`tests/test_button_press_classifier.c`, 7 cases).
- `zigbee_app.[ch]` — application-facing wrapper around the ncs-zigbee
  R23 stack. Exposes `init / start_join / factory_reset / is_joined` so
  ZBOSS API details stay out of `main.c`. Signal handler is wired to
  `zigbee_default_signal_handler` from `zigbee/zigbee_app_utils.h`.
- `main.c` — button IRQ refactored to `GPIO_INT_EDGE_BOTH`, tracks
  press start/release, publishes the duration via a semaphore, and a
  low-priority thread classifies + dispatches to `zigbee_app`. The
  falling-edge branch still increments `bench_pulse_count` so #7's
  LPCOMP swap can remove that path cleanly.
- `prj.conf` — Zigbee stack Kconfig: `ZIGBEE`, `ZIGBEE_ROLE_END_DEVICE`,
  `ZIGBEE_APP_UTILS`, `SETTINGS`.

## Press-classification bands

| Duration | Kind | Action |
|---|---|---|
| 0 ms (chatter) | NEITHER | ignored |
| 1–999 ms | SHORT | `bdb_start_top_level_commissioning(NETWORK_STEERING)` |
| 1000–2999 ms | NEITHER | ignored (deliberate gap) |
| ≥3000 ms | LONG | factory reset |

Design doc says short = <1 s, long = >3 s. The gap between is the
"whoops I released mid-way" band — accidentally landing in it neither
joins nor resets, which is much safer than making the boundary sharp.

## What was NOT done in this slice

The `zigbee_app.c` implementation has two blocks marked
`TODO(ncs-zigbee-api)`:

1. **Cluster + endpoint declarations** — the ZBOSS macros for
   `Basic (0x0000)` + `Identify (0x0003)` attribute lists and the
   device-context declaration. Straight-copyable from ncs-zigbee's
   `samples/light_switch/light_bulb` once the west workspace (#3)
   is in.
2. **`ZB_AF_REGISTER_DEVICE_CTX` + `ZB_ZCL_REGISTER_DEVICE_CB`** —
   registering the declared context and the identify callback that
   drives the red LED for identify-effect commands.

The `bdb_start_top_level_commissioning`, `zb_bdb_reset_via_local_action`,
and `zigbee_configure_sleepy_behavior` calls are named per my best
recollection of the ncs-zigbee R23 API; if any of these have been
renamed for 1.3.0 the compile error will point straight at them.

## What was NOT bench-verified

- Device pairing to a real Z2M coordinator within 30 s of a short-press.
- The Identify command from Z2M actually blinking the red LED.
- Long-press factory reset actually clearing the network state so the
  next short-press re-joins fresh.

All of these need a real board + a running Z2M coordinator, neither of
which are available in this sandbox. The button-driven state machine
is unit-tested up to the point where it calls into `zigbee_app`; the
`zigbee_app` side needs the header-verified fill-in above plus a bench
session to close out the acceptance criteria.

## Coordination with #7

`main.c`'s button IRQ still calls `atomic_inc(&bench_pulse_count)` on
falling edge. Issue #7 (LPCOMP hardware pulse chain) will remove that
line + the `bench_pulse_count`/`pulse_edge_detector` sample-loop path
once the hardware counter is wired. This PR intentionally leaves the
scaffolding intact so #7 can be a clean, non-conflicting delete.

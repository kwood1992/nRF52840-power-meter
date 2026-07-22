# NVS persistence for the pulse accumulator (issue #2)

## What was added

- `nvs_store.[ch]` — thin wrapper over Zephyr NVS on the DT
  `storage_partition` (0x000ec000, 32 KB). Loads/stores a single u64 for
  the accumulator's cumulative total.
- `persist_policy.[ch]` — pure-logic helper deciding *when* to write.
  Host-tested (`tests/test_persist_policy.c`) so the policy stays
  decoupled from Zephyr's NVS API.
- `main.c` boot path — on startup, mounts NVS and if a saved total exists,
  passes it to `pulse_accumulator_restore()` before the sample loop
  starts. Sample loop then calls `persist_policy_should_write()` each
  iteration and saves when the policy fires.
- `prj.conf` — `CONFIG_FLASH`, `CONFIG_FLASH_MAP`, `CONFIG_FLASH_PAGE_LAYOUT`,
  `CONFIG_NVS`.

## Persistence policy

Write to NVS on whichever fires first:

1. **Wall-clock cadence**: every 5 min (`PERSIST_INTERVAL_MS`).
2. **Pulse-delta safety net**: every 100 counted pulses since last save
   (`PERSIST_MAX_PULSE_DELTA`).

And never write if the total hasn't changed.

**First-persist after boot**: `main.c` backdates `last_saved_ms` by the
full interval so the very first counted pulse after boot triggers an
immediate write. Without this, a bench cycle of "press < 100 times,
reboot within 5 min" never fires either safety net — NVS stays empty
and cold boot looks identical to a broken mount. Costs one extra write
per boot in the field, negligible against the 288/day budget.

## Why these numbers

**5-min cadence** — aligns with the design doc's 5-min Zigbee report
interval, so eventually one wake pays for both a report and a persist.
Flash wear budget: 12 writes/hour × 24 = 288/day. With 8 sectors on
`storage_partition` (32 KB / 4 KB) rotating and roughly 10k cycles per
sector, that's over 7 years of headroom — well past the multi-year AAA
target from the design doc.

**100-pulse safety net** — at the default `Divisor=1000` (imp/kWh),
100 pulses = 0.1 kWh. That's a tolerable worst-case data-loss bound on
an unexpected reset under a burst (bench button-mashing, fault light
going haywire). Trades a small number of extra flash writes during
bursts for bounded loss.

## What was NOT verified

- **First bench cycle observed the "cold boot" trap**: flashed board
  logged `no persisted accumulator total — cold boot` after every
  reset because neither safety net had fired between resets
  (< 100 presses, < 5 min uptime). Fixed by backdating `last_saved_ms`
  at boot (see policy note above). Re-verify by:
  - Press button once, watch for `persisted accumulator_total=1`.
  - Reset, watch for `restored accumulator_total=1 from NVS`.
  - Continue pressing; steady-state cadence should now be 5-min or
    100-pulse, whichever fires first.
  - Optional stress: press button > 100 times in quick succession
    without waiting for the wall-clock; confirm the pulse-delta safety
    net fires.

## Related

Issue #4 (Zigbee join) and issue #7 (LPCOMP hardware pulse chain) both
touch `main.c`'s button IRQ handler / sample loop. The persistence hooks
in this PR sit in the sample loop right after `pulse_accumulator_update`,
so they compose cleanly with #7 replacing the counter source (software →
LPCOMP+PPI+TIMER register) — the persist policy operates on
`pulse_accumulator_total()`, which is source-agnostic.

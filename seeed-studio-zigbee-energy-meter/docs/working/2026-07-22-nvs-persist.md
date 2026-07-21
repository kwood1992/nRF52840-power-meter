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

- **No bench verification.** The board wasn't flashed as part of this
  slice. The acceptance-criteria "press button N times, hit reset,
  confirm log continues from N" needs a real device. When flashing:
  - Watch for `restored accumulator_total=X` on cold boot (post-reset).
  - Press button > 100 times to trigger the pulse-delta safety net;
    confirm a `persisted accumulator_total=X` log appears.
  - Wait 5+ min without pressing; confirm the wall-clock write fires
    if the total has advanced.

## Related

Issue #4 (Zigbee join) and issue #7 (LPCOMP hardware pulse chain) both
touch `main.c`'s button IRQ handler / sample loop. The persistence hooks
in this PR sit in the sample loop right after `pulse_accumulator_update`,
so they compose cleanly with #7 replacing the counter source (software →
LPCOMP+PPI+TIMER register) — the persist policy operates on
`pulse_accumulator_total()`, which is source-agnostic.

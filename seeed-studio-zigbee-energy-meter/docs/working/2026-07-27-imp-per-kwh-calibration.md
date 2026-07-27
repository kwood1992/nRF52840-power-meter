# Runtime imp/kWh calibration (issue #48)

## What shipped

- `src/calibration.[ch]` — pure predicate `calibration_is_valid_imp_per_kwh`.
  Range `[100, 10000]`. Host-tested (7 cases in
  `tests/test_calibration.c`). No state, no allocations — the ZCL write
  handler calls this synchronously on every write attempt.
- `src/nvs_store.[ch]` — new key `NVS_ID_IMP_PER_KWH = 2` plus
  `nvs_store_{load,save}_imp_per_kwh` helpers. Same
  `nvs_read`/`nvs_write` shape as the existing accumulator slot;
  4 bytes on the wire. IDs are stable schema — the comment now spells
  that out so a future PR doesn't renumber and orphan on-device data.
- `Kconfig` — `CONFIG_APP_METERING_DEFAULT_IMP_PER_KWH` (int,
  default 1000, range 100–10000). Cold-boot fallback. Range enforced
  in Kconfig so a nonsense default is rejected at build time, not
  runtime.
- `src/zigbee_app.c` — the meat. Detailed below.

## Why the standard-attribute path took work

The design-doc row for the Zigbee model says
`Divisor=1000 (imp/kWh, adjustable)`, and the original comment in
`zigbee_app.c` promised runtime writes via a Z2M attribute write. That
comment was wrong: ZBOSS's stock `ZB_SET_ATTR_DESCR_WITH_ZB_ZCL_ATTR_METERING_DIVISOR_ID`
macro (in `zboss/production/include/zcl/zb_zcl_metering.h:2438-2445`)
declares Divisor with `ZB_ZCL_ATTR_ACCESS_READ_ONLY`, which is what the
ZCL spec says. `ZB_ZCL_DECLARE_METERING_ATTRIB_LIST_EXT` uses that
macro under the hood, so any device built via the EXT list inherits the
read-only access flag and rejects writes at the ZCL layer before our
device callback ever fires.

## The two-part fix on the firmware side

**1. Hand-roll the attribute list so Divisor gets `READ_WRITE`.**
`ZB_ZCL_SET_ATTR_DESC_M(id, data_ptr, type, attr)` (defined in
`zb_zcl_common.h:846`) lets us insert one attribute with a custom
access-flag override while every other attribute stays on the stock
`ZB_ZCL_SET_ATTR_DESC` (which pulls in the read-only default). The
result:

```c
ZB_ZCL_START_DECLARE_ATTRIB_LIST_CLUSTER_REVISION(metering_attr_list,
                                                  ZB_ZCL_METERING)
    ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_CURRENT_SUMMATION_DELIVERED_ID, ...)
    /* ...eight more stock read-only entries... */
    ZB_ZCL_SET_ATTR_DESC(ZB_ZCL_ATTR_METERING_MULTIPLIER_ID, ...)
    ZB_ZCL_SET_ATTR_DESC_M(ZB_ZCL_ATTR_METERING_DIVISOR_ID,
                           &dev_ctx.metering_divisor,
                           ZB_ZCL_ATTR_TYPE_U24,
                           ZB_ZCL_ATTR_ACCESS_READ_WRITE)
ZB_ZCL_FINISH_DECLARE_ATTRIB_LIST;
```

Multiplier is kept read-only deliberately: two-variable
`kWh = raw × Mult ÷ Div` is harder to reason about than one-variable
`kWh = raw ÷ Div`. The imp/kWh scope owns Divisor, nothing else.

**2. Register a ZCL device callback for `ZB_ZCL_SET_ATTR_VALUE_CB_ID`
that validates + persists + rolls back.** ZBOSS invokes the callback
*after* the attribute has already been written to the attribute store,
so out-of-range values need active rollback (via `ZB_ZCL_SET_ATTRIBUTE`
with `check_access=ZB_FALSE`, bypassing the RW gate for our own
recovery write). The valid path calls `nvs_store_save_imp_per_kwh`.

On boot, `metering_attrs_init` loads the persisted value, falls back to
`CONFIG_APP_METERING_DEFAULT_IMP_PER_KWH` on `-ENOENT`, and warns +
falls back on out-of-range NVS values (defensive against a
future firmware that widened the range and then narrowed it again).

## The Z2M-side surprise (documented for future you)

The firmware side is spec-adjacent (a standard-cluster attribute with
an app-flipped access flag), and it works fine when ZBOSS receives the
write. The problem is *getting the write there*.

zigbee-herdsman-converters' generic write path in Z2M pre-checks each
named attribute against the ZCL spec's access table before it sends
anything on the wire. For Metering.Divisor — spec-read-only — it
throws:

    Error: Status 'NOT_AUTHORIZED' divisor (770) is not writable

**even though our device would accept the write.** The pre-check is
purely client-side; the wire never gets touched.

The bypass is to address the attribute by numeric ID rather than name:

    {"write":{"cluster":"seMetering","payload":{"770":{"value":800,"type":34}}}}

ZHC's writable-attribute check keys off the *field name* (matching
against known-attribute names in its spec table). Numeric IDs skip that
lookup entirely, and the write reaches our device untouched. Confirmed
against Z2M 2.12.1 + ZStack3x0 on 2026-07-27.

`770 = 0x0302`, `34 = 0x22` (ZCL uint24). Documented for users in the
top-level README's "Calibrating imp/kWh" section and in memory
`reference_z2m_write_read_only_attr_bypass.md`.

## Sleepy-ED write-timing gotcha

Steady-state long-poll is 60 s. Z2M's write deadline is 10 s. Any
write attempted outside the 30 s post-reboot turbo-poll window will
time out at the herdsman layer with the value never landing.

Workaround for users today: short-press the device button (fresh
join → turbo-poll re-opens) before writing. Documented in the README
section.

This is a UX wart, not a bug. A proper "wake on write" mechanism —
either a button-triggered turbo-poll refresh, or a bind-configured
Poll Control cluster driven by Z2M — is the future path. See the
follow-up issue for the Z2M-side slider work; that issue is the
natural place to also fix this.

## Rollback semantics

Invalid writes (below 100 or above 10000, plus 0) get rolled back
in-place but ZBOSS's Write Attributes Response still reports the
underlying write as SUCCESS to the coordinator. Result: Z2M logs
`zhc:tz: Wrote '...' to 'seMetering'` even for a rejected value; only
the readback exposes the rollback. Bench-verified: write 50, readback
returns previous valid value.

This is because the ZBOSS invocation is post-write, not veto-style —
`p->status = RET_OUT_OF_RANGE` set inside the callback influences the
callback's return path but doesn't retroactively rewrite the wire
response. For our use case (installer types a wrong value, gets no
change on readback) this is acceptable; a stricter semantic would
need a pre-write hook that ZBOSS doesn't expose.

## Bench verification (2026-07-27, on the SWD-flash + Z2M rig)

- `Read seMetering.divisor` after factory reset → `1000` (compile
  default, NVS empty).
- `Write payload {"770":{"value":800,"type":34}}` inside turbo-poll
  window → ZHC logs `Wrote`. Readback → `800`.
- SWD reset (preserves NVS partition). Readback → `800`. NVS
  persistence works.
- Write `{"770":{"value":50,"type":34}}` (below MIN). Z2M logs
  `Wrote` (the wire-response wart above). Readback → still `800`
  (rollback works).

Not yet run on-bench: the full "N pulses × new divisor →
matching kWh in Z2M" chain. The divisor value transits correctly
(readback matches what was written), and the pulse-to-kWh math lives
in Z2M's `electricityMeter` modernExtend server-side using that
divisor, so end-to-end correctness follows from the two independently
verified paths. If a real-meter deployment ever contradicts this,
first check readback matches expected divisor before suspecting the
firmware.

## Follow-ups

- Z2M-side slider (external-converters extension). Numeric-ID writes
  from the UI work but are un-discoverable. A `m.numeric()` extend
  with a custom toZigbee handler that calls `endpoint.write` with the
  numeric-ID form would make the imp/kWh knob appear on the device
  page. Filed as a separate issue.
- Sleepy-ED wake-on-write. See above.
- Fold in Multiplier writability if a future meter needs
  non-1 multipliers (unlikely — most residential meters are integer
  imp/kWh). Just add a second NVS slot and a second callback branch.

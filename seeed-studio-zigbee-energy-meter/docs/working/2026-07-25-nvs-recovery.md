# NVS init: recover from mount failure via nvs_clear retry (issue #15)

## What changed

`nvs_store_init()` in `src/nvs_store.c` now catches `nvs_mount` failure,
calls `nvs_clear()` on the same `nvs_fs` instance, and retries `nvs_mount`
once. On recovery success we log `WRN`:

```
recovered NVS after mount failure — accumulator reset to 0
```

and `initialized = true` — persistence works for the rest of this boot,
though `nvs_store_load_total()` will return `-ENOENT` because the
partition was wiped. On second failure we return the mount error as
before; `main.c:351` already logs `ERR` and continues without persistence
(unchanged behavior).

## Why only nvs_mount gets the retry

The three failure paths in `nvs_store_init` are not equal:

- `device_is_ready(fs.flash_device)` — flash driver not registered.
  Dev-time misconfig; a shipped firmware either has the driver or it
  doesn't. Not field-recoverable, no retry.
- `flash_get_page_info_by_offs` — DT partition offset not on a page
  boundary. Same story: build-config bug, dev-time only.
- `nvs_mount` — partition contents inconsistent with what NVS expects
  (prior firmware layout, partial write from a cut power event, bit rot
  in sector headers). Realistic on a physically-installed device;
  `nvs_clear` + reformat almost always recovers.

Retrying the first two would waste code and mask a fault we want to
catch at bench time. See the "Rationale" section of issue #15 for the
decision-tree exchange.

## What happens on double failure

Second `nvs_mount` still errors → `nvs_store_init` returns the error
code → `main.c` logs `LOG_ERR("nvs_store_init failed: %d — proceeding
without persistence"...)` and continues. This is the same end state as
before the change, so we don't make things worse for a device with a
truly dead partition — we just get one extra recovery attempt for
devices that were previously stuck in perpetual data loss.

## Not verified

- No new host tests: `nvs_store` isn't host-testable (Zephyr flash +
  NVS deps), same as when it was introduced in #2. Field verification
  requires deliberately corrupting a sector header, which needs the SWD
  workflow.
- Build succeeds cleanly — see the PR checks.

## Follow-up

Issue #5 (metering-cluster diagnostic attributes) is the natural home
for a "persistence healthy" bit that HA / Z2M can surface to users — so
a device that hits the `initialized = false` path after retry can raise
its hand rather than silently pretending everything is fine. Not in
scope for this change.

# Poll Control (0x0020) — unattended writes work (#62 option B)

Date: 2026-07-30
Firmware: `feat/62-poll-control-unattended-write`
Z2M 2.12.1, ZStack3x0 coordinator.

Option A (#62, merged in #66) fixed the human-present case: short-press, then
write. This closes the case A explicitly could not — **a write with nobody at
the device**, which is what a Home Assistant automation needs.

## Why it works — the herdsman side

Established by reading zigbee-herdsman `src/controller/model/device.ts`
before writing any firmware, because the whole approach is worthless if the
coordinator doesn't play along. Two independent behaviours, both keyed off the
device merely *exposing* `genPollCtrl`:

1. **Requests get queued instead of failing.**
   ```ts
   private resetPendingRequestTimeout(): void {
       this._pendingRequestTimeout = (this._checkinInterval ?? 0) * 1000;
   }
   ```
   Devices supporting `genPollCtrl` default to 86,400,000 ms (1 day) until
   herdsman has read our CheckInInterval. So a write no longer dies at the
   10 s ZCL deadline — it sits in a queue.

2. **The queue is drained when we make contact.**
   ```ts
   if (this.hasPendingRequests() || this._checkinInterval === undefined) {
       // → checkinRsp {startFastPolling: 1}
       await Promise.all(this.endpoints.map(async (e) => await e.sendPendingRequests(true)));
   }
   ```

Herdsman also **binds `genPollCtrl` to the coordinator automatically** during
interview — confirmed on the bench, see below. The `m.bindCluster` addition to
the external converter is therefore belt-and-braces, not load-bearing, which
is a good thing: it means option B works on an unmodified converter install.

## Firmware side

- Poll Control server added as a 4th server cluster on endpoint 10
  (`zb_meter_ep.h`), declared unconditionally — gating it would need a second
  cluster list *and* a second simple descriptor, since the cluster count is
  baked into both macros.
- `poll_control_attrs_init()` seeds CheckInInterval / LongPollInterval /
  FastPollTimeout from Kconfig. All ZCL Poll Control intervals are in
  **quarter-seconds**; every assignment goes through an explicit conversion.
- The Poll Control attributes are now the **single source of truth** for poll
  cadence, and `apply_sleepy_poll_intervals_if_joined()` drives the PIM *from
  the attribute table* rather than straight from Kconfig. This is the conflict
  the ticket flagged as a risk: previously a coordinator writing
  LongPollInterval would be silently overwritten on the next join.

### Two SDK traps hit here

**`known_issues.rst` is written against ZBOSS internals.** Its workaround
snippet ("Apply Poll Control values loaded from NVRAM to the PIM") calls
`zb_zdo_pim_set_fast_poll_interval()` and `zb_zdo_pim_set_fast_poll_timeout()`.
**Neither exists in ncs-zigbee v1.3.0's public headers** — they're behind
`ZB_USE_INTERNAL_HEADERS`. Only `zb_zdo_pim_set_long_poll_interval()` is
callable, which is also the only one the doc's own rationale applies to (the
fast-poll path is driven by the cluster from its attributes). So we push long
poll only.

**The macro name is misspelled in ZBOSS.** It is
`ZB_QUARTERECONDS_TO_MSEC` (`zboss_api_core.h:288`) — no `S` in "QUARTER*S*ECONDS".

## Z2M had to be removed and re-paired

First re-join after the firmware change reported `interview_state: SUCCESSFUL`
but Z2M still listed only `['genBasic', 'genIdentify', 'seMetering']`. Z2M
serves endpoint/cluster data from its own database and a device-side factory
reset does **not** make it re-read the simple descriptor. `z2m-cli remove`
followed by a fresh join was required.

After that:

```
endpoint 10
  input clusters : ['genBasic', 'genIdentify', 'seMetering', 'genPollCtrl']
  bindings       : [('genPollCtrl', 'endpoint'), ('seMetering', 'endpoint')]
```

The `genPollCtrl` binding appeared **without** the updated converter being
installed on the HA host — that's the automatic herdsman bind.

This generalises the stale-state warning already recorded for #57: any change
to the endpoint's cluster list needs a Z2M remove + re-pair, not just a
factory reset.

## Bench result — n=3 unattended operations, zero errors

All performed in genuine steady state (130 s after join, turbo window expired)
with **no button press at any point**:

| # | Operation | Result |
|---|---|---|
| 1 | write `{"imp_per_kwh": 750}` | landed, 800 → 750 |
| 2 | read `min_pulse_width_us` | landed, returned 2500 |
| 3 | write `{"imp_per_kwh": 800}` (restore) | landed, 750 → 800 |

**Total `"level":"error"` count in the Z2M log across the whole session: 0.**

Compare the control from the option-A session on the same operation:
`Timeout after 10000ms`. The 10 s deadline is simply no longer in play.

### Observed latency, and why it beats the check-in interval

Operation 1 was issued at 03:38:18 and landed inside the 400 s observation
window — i.e. **faster than the 900 s CheckInInterval**. So delivery was not
check-in driven. herdsman drains the pending queue on *any* incoming message
from the device, and our metering report runs every 5 minutes, so:

> unattended write latency ≈ min(next metering report, next check-in)
> ≈ **≤5 minutes** in the current configuration.

The 15 min check-in is the backstop for when reporting is idle, not the normal
path. Useful consequence: lowering `APP_ZIGBEE_CHECKIN_INTERVAL_S` further
would buy nothing while the 5-minute report exists.

## `min_pulse_width_us: null` — investigated, not a regression

Immediately after the fresh interview the state published
`min_pulse_width_us: null`. An explicit read returned **2500**, the correct
persisted value. The interview-time read of the manufacturer-specific
attribute simply hadn't populated it; the firmware value was intact
throughout. Worth knowing as a re-pair cosmetic, not a bug.

## Rig state left behind

- Joined, interview SUCCESSFUL, `imp_per_kwh` restored to **800**,
  `min_pulse_width_us` 2500, `energy` 3504.04.
- `permit_join` off, all bench MQTT subscribers stopped, no openocd running.
- **Z2M device history was reset** by the remove + re-pair. Unavoidable given
  the cluster-list change.

## Battery cost

96 check-ins/day at 900 s, each one short ZCL exchange. Against the sub-50 µA
sleep floor from #58 this is noise, and no INA219 run can resolve it (±300 µA
single-sample noise). Not measured, for the same reason as #66's window cost.

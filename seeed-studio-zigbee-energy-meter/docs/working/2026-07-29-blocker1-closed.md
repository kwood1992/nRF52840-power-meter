# Blocker #1 (sleepy-ED) closed by direct RTT probe + INA219 rerun (#8, #51)

Follow-up to `2026-07-29-ina219-baseline-diagnosis.md`. That session left
blocker #1 open with the working hypothesis "ZBOSS is silently reverting
`rx_on_when_idle` to TRUE after join". Once the RTT-over-SWD path from
#51 landed, we could read `zb_get_rx_on_when_idle()` directly from a
battery-mode run and check that hypothesis against live device state
instead of inferring it from the current profile alone.

Result: hypothesis is wrong, blocker #1 is closed.

## Probe evidence

Firmware built with `-DEXTRA_CONF_FILE=rtt.conf` (turns on
`APP_ZIGBEE_RX_IDLE_PROBE`), flashed via `flash-swd.sh`,
`rtt-tail.sh baseline-rx-idle-diag-v2` capturing to
`rtt-2026-07-29-142826-baseline-rx-idle-diag-v2.log`. Sequence: factory
reset via long-press at t≈17 s, fresh join at t≈29 s, 5 min of idle
joined operation afterward.

```
[00:00:05.023]  post-configure-sleepy   FALSE (joined=0)   — pre-enable sanity
[00:00:13.783]  post-reboot-reattach    FALSE (joined=1)   — reattached to prior NVS network
[00:00:17.474]  factory reset triggered (long-press)
[00:00:29.492]  post-steering           FALSE (joined=1)   — FRESH JOIN
[00:01:29.493]  tick                    FALSE (joined=1)   — +60 s post-join
[00:02:29.494]  tick                    FALSE (joined=1)   — +120 s
[00:03:29.494]  tick                    FALSE (joined=1)   — +180 s
[00:04:29.495]  tick                    FALSE (joined=1)   — +240 s
[00:05:29.495]  tick                    FALSE (joined=1)   — +300 s
```

Every probe point across a fresh join and 5 min of steady-state operation
reports FALSE. ZBOSS is not clobbering our setting. That hypothesis
is dead.

## INA219 evidence

Same rig as the 2026-07-29 baseline (XIAO on Pi 3V3 through the shunt,
USB unplugged). Two 6-minute runs, both with a factory-reset-and-rejoin
in the first 30 s, distinguished only by whether `rtt-tail.sh` was
attached (which drives openocd's SWD-DAP polling continuously):

| condition | 30 s onward mean | p50 | p95 | sd |
| --- | ---:| ---:| ---:| ---:|
| **`baseline-rx-idle-diag-v2`** (rtt-tail attached) | 1.83 mA | 1.80 | 2.10 | 0.15 |
| **`baseline-no-rtt-tail`** (SWD idle) | 1.64 mA | 1.60 | 1.90 | 0.16 |

Compare with the 2026-07-29 `settled-postjoin` baseline: mean 12.43 mA,
sd 0.17 — dead flat at radio-always-on. Today's steady-state is ~7×
lower and *does* show the sleepy-ED signature (occasional 8–35 mA spikes
in the max column matching parent-poll bursts; median in the 1.6–1.8 mA
band between polls).

CSVs: `ina219-2026-07-29-142831-baseline-rx-idle-diag-v2.csv`,
`ina219-2026-07-29-144243-baseline-no-rtt-tail.csv`.

The 12.43 mA baseline from 2026-07-29 is **not reproducing today** on
the same firmware branch (`main` + `rtt.conf` overlay is functionally
the same sleep-path code that shipped as `main`@`ea28e49`). Best guess
for what changed: USB may have actually been plugged in during the
2026-07-29 run despite the doc saying "unplugged" — the flat 12 mA
signature matches "radio in continuous RX", which lines up with
`rx_on_when_idle=TRUE` behaviour, which ncs-zigbee's
`zigbee_configure_sleepy_behavior()` would leave TRUE if
`APP_ZIGBEE_SLEEPY_ED=n` had somehow been in effect. Not proven, but the
probe evidence says the current firmware path definitely works.

## SWD polling overhead

The `rtt-tail`-attached vs `rtt-tail`-detached delta is **+0.19 mA**
(11 %) — measurable, but small enough that RTT logging can stay
attached during future diagnostic runs without meaningfully distorting
the reading. Just note the offset in the run label.

## Residual 1.64 mA — new investigation, not blocker #1

The 30-s-onward median of 1.6 mA lines up almost exactly with the
nRF52840 datasheet's "CPU System-ON idle, HFCLK on" figure (~1.6 mA).
Something is keeping HFCLK running even without SWD polling. Two most
likely culprits (both in the "known blockers" list of the
2026-07-25 bench-prep doc, both still open):

- **USB device stack** (`CONFIG_USB_DEVICE_STACK=y`, blocker #2). USB
  PHY needs HFCLK; the driver may hold HFCLK on for as long as it's
  initialised, regardless of whether USB is enumerated. `rtt.conf`
  redirects logs off CDC-ACM but doesn't disable the stack itself.
- **TIMER2 in counter mode for the pulse chain** — TIMER peripherals
  are clocked from PCLK16M which is derived from HFCLK. Investigation
  needed to see whether TIMER2 in `MODE_COUNTER` actually needs
  HFCLK running, or whether the PPI event path holds it up
  independently.

The bench-prep doc already orders these as blockers #2 and beyond.
Blocker #1's job was to prove `rx-off-when-idle` actually happens on
the wire — it does. The residual current is the *next* thing to attack.

## Decision

Close blocker #1 (`zigbee_configure_sleepy_behavior` / rx-on-when-idle
plumbing) — the ncs-zigbee R23 sleepy-ED path works as documented,
`APP_ZIGBEE_SLEEPY_ED=y` is the correct default, and the shipped fix
from PR #46 (turbo-poll-during-join + `zb_set_ed_timeout` +
`zb_set_keepalive_timeout`) is delivering. Next: blocker #2 (USB
device stack) — measure whether disabling `CONFIG_USB_DEVICE_STACK=y`
on the RTT/battery build drops the residual toward System-ON deep
sleep.

## Products of the session

Landing on the tree, ready to commit:

- `docs/working/rtt-2026-07-29-142826-baseline-rx-idle-diag-v2.log` —
  8.5 KB RTT capture with the probe evidence above. Fresh join with
  5 minutes of steady-state ticks.
- `docs/working/ina219-2026-07-29-142831-baseline-rx-idle-diag-v2.csv`
  — INA219 profile paired with the RTT log (SWD polled).
- `docs/working/ina219-2026-07-29-144243-baseline-no-rtt-tail.csv` —
  isolation run, SWD wires idle.
- This doc.

Also committed separately as PR #53: `nc -d` fix in `tools/rtt-tail.sh`
(BSD nc closed the socket on EOF from a piped-stdin parent, so the
first attempt at concurrent RTT + INA capture produced a 0-byte RTT
log — hence the `-v2` suffix on the good one).

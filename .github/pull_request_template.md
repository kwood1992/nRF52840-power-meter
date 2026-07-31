<!--
Keep PRs small and single-purpose. See CONTRIBUTING.md for the full process.
-->

## What and why

<!-- What changes, and what problem it solves. Link the issue: "Closes #NN". -->

## Design-doc reference

<!--
Which locked decision or working note does this implement?
If it *changes* a locked decision, say so explicitly and update the design
doc in the same PR — a divergence that only lives in code is a trap for the
next person.
-->

## Testing

- [ ] Host tests pass (`make -C seeed-studio-zigbee-energy-meter/tests test`)
- [ ] Tooling tests pass, if `tools/` changed
- [ ] New pure logic was written test-first (project rule — see CONTRIBUTING)
- [ ] Firmware builds for the target(s) affected

### Bench verification

<!--
Hardware paths (LPCOMP, PPI, radio, USB, GPIO, NVS) cannot be unit-tested,
so they need bench evidence. A compile is not verification.

Say what you actually ran and what you observed. Paste RTT/serial output and
Z2M behaviour. If this PR is pure logic or docs and touches no hardware path,
write "N/A — no hardware path touched" and skip the power section below.
-->

### Power numbers

<!--
REQUIRED for any firmware change. This device is meant to run for years on
two AAA cells, so current draw is a product requirement, not a nice-to-have —
and it is the one regression that no test, review or CI check will catch for
you. It shows up months later as dead batteries.

Report measured current before and after your change:

| | Baseline (main) | This PR |
|---|---|---|
| Sleep floor (System-ON, idle) | | |
| Average over a representative window | | |

Also state:

- How you measured it (INA219 harness, meter, window length)
- Whether the board was power-cycled — a POR is non-deterministic and fails
  to take roughly half the time; a failed POR reads as a tight ~1.5 mA peak
- Which controls you subtracted. The D7 bench pull-up alone adds ~0.242 mA
  during pulse runs, so an uncontrolled pulse measurement overstates the cost
  by roughly 6x

"No measurable change" is a fine result — but say it because you measured,
not because you expect it. If the change genuinely cannot affect power (a
string, a log line, a host-side test), say which and why.
-->

## Cluster-list impact

<!--
Did the Zigbee endpoint's cluster list change?

If yes: the Z2M external converter has to move in step, AND anyone with a
paired device must remove and re-pair it — Z2M serves cluster data from its
own database and will not re-read the simple descriptor after a device-side
factory reset. Call that out in the PR description so it reaches the
changelog.

If no: "No change."
-->

## Risks and follow-ups

<!-- Anything left incomplete, deliberately deferred, or worth watching. -->

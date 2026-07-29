# POR test result — SWD-attach WAS the anchor. Deep sleep floor 0.1-0.2 mA (#8)

The definitive test from `2026-07-29-peripheral-audit.md`: with SWD
wires detached AND the XIAO power-cycled (breaking the Pi 3V3 → BAT
wire briefly), does INA219 current drop below the ~1.6 mA floor?

Answer: **yes.** Every 180 s post-POR sample includes multiple
sub-mA windows and at least one deep-sleep hit at 0.1-0.2 mA.

## POR #1 (SWD detached + 3.3V briefly interrupted)

`ina219-2026-07-29-174120-post-por.csv`:

- mean 8.635 mA, sd 1.202 mA
- min **0.100 mA** — the true System-ON sleep floor
- p50 9.000 mA, p95 9.200 mA, max 16.400 mA
- 16 sub-mA samples over 180 s

## POR #2 (repeat after RST-pin experiments)

`ina219-2026-07-29-181953-post-por2.csv`:

- mean 8.693 mA, sd 1.326 mA
- min **0.200 mA**
- p50 9.200 mA, p95 9.400 mA, max 17.600 mA
- Same shape — scan/rejoin dominates, brief sleep dips visible

## Interpretation — the 1.6 mA WAS SWD-attach

The pre-POR baselines with SWD attached (or physically disconnected
but never power-cycled) were all consistent at 1.60-1.65 mA. The
distributions were tight, unimodal — behavior of a fixed hardware
load, not a firmware duty-cycle pattern.

Post-POR, the distribution is completely different — bimodal, with a
bright 9 mA peak (scan/RX-continuous) and rare deep-sleep dips to
100-200 µA. That means:

1. **CDBGPWRUP is really a latched DP-register state that survives
   SWD wire disconnect.** Only Vdd cycling clears it. This matches
   the ARM ADIv5 spec — `dbgreset` is asserted by POR or explicit
   DAP write, not by SWD electrical disconnect.

2. **The firmware sleep architecture is correct.** The 100-200 µA
   deep-sleep hit is close to Seeed's spec for the XIAO Sense
   ("standby < 5 µA" per the wiki, plus module-level LDO/charge-IC
   overhead in the tens of µA range).

3. **The 1.65 mA anchor we chased across 5 working docs was the
   ARM CoreSight debug interface holding HFCLK on continuously via
   the CSYSPWRUPACK path.** No firmware anchor exists.

## Why we couldn't get the definitive joined-sleepy-ED number today

To measure the true joined-and-idle current, we'd need:

1. Physically detached SWD + POR.
2. Device in joined-and-idle-polling state with Z2M knowing about it.

After the POR the device lost its Z2M association. Attempts to
re-join failed — the device fell into a rejoin-scan mode (~9 mA
continuous RX with brief sleep windows for orphan-scan channel
hopping). Long-press factory-reset via the Pi's button-injection
GPIO didn't trigger a fresh BDB steering — likely because
`zb_bdb_reset_via_local_action` needs a working parent to send LEAVE
to before wiping NVRAM.

RST-pin experiments (BCM 23 → XIAO RST) either landed the device in
Adafruit UF2 bootloader mode (~18 mA) or the same rejoin-scan mode.

To progress: either

- Get the device into a fresh join state with Z2M knowing about it
  (probably by editing Z2M config to allow the device by IEEE
  regardless of past interview state, then triggering a factory
  reset). Then physically POR + measure.

- Or use a PPK2 rig where SWD isn't involved in the power measurement
  path — INA219 rig is fundamentally limited by needing power to flow
  through the Pi, which requires the Pi to be up and reachable, which
  keeps the temptation to leave SWD attached alive.

## Rig upgrade proposed for next session

User is adding a **relay in the Pi 3V3 → XIAO BAT power path**,
controlled by Pi BCM 22 (physical pin 15). Use the relay's **NC
(Normally Closed)** terminal:

- Pi 3V3 → NC → COM → XIAO BAT (default: relay de-energized = power on)
- Coil driven by Pi BCM 22

This makes POR a scriptable Pi-side command:

```
pinctrl set 22 op dh   # energize coil → NC opens → power cut
sleep 2                # true POR window
pinctrl set 22 dl      # de-energize → power restored
pinctrl set 22 ip pu   # release GPIO
```

Combined with the "physically detach SWD" step (still one-time
manual), that turns the flash-measure loop into a fully-remote
workflow.

Once the relay is in, the workflow is:

1. Flash firmware via SWD (short OpenOCD session, re-asserts
   CDBGPWRUP briefly).
2. Detach SWD (one-time; leave detached for the whole measurement
   campaign).
3. `pinctrl 22` POR sequence → clears CDBGPWRUP latch.
4. INA219 sample.
5. Re-attach SWD only when we need to flash again.

## Landing decision

Commit CSVs + this doc + memory update. The core question is
answered: firmware sleep works, SWD-attach was the anchor. Future
sessions with the relay in place can measure the true joined-idle
current (predicted sub-500 µA average with brief 60 s poll spikes).

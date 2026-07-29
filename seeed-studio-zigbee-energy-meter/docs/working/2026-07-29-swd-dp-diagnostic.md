# SWD-side diagnostic inverts the anchor story — HFCLK is OFF (#8)

Follow-up to `2026-07-29-timer-probe-result.md`. That doc left the
1.65 mA floor pointing at SWD-attach as the suspected anchor. Two
SWD-side reads via OpenOCD confirm:

## Reads

Via `sudo openocd -f interface/raspberrypi-swd.cfg ... nrf52.cfg`:

```
nrf52.dap dpreg 4      → 0xf0000041
mdw 0x40000408 (HFCLKSTAT) × 5 (over ~5.5s) → 0x00000000 (all)
mdw 0x40000104 (HFCLKRUN)  → 0x00000000
mdw 0x40000418 (LFCLKSTAT) → 0x00010001
mdw 0xE000EDF0 (DHCSR) × 4 → 0x00050001 (all)
nrf52.dap dpreg 4      → 0xf0000041
```

## Decoded

**DP CTRL/STAT = 0xf0000041**
- Bit 31 CSYSPWRUPACK = 1
- Bit 30 CSYSPWRUPREQ = 1
- Bit 29 CDBGPWRUPACK = 1
- Bit 28 CDBGPWRUPREQ = 1
- Bit 6  READOK       = 1
- Bit 0  ORUNDETECT   = 1

Both system + debug power domains asserted UP by OpenOCD's DAP init.
Expected during an active OpenOCD session.

**HFCLKSTAT = 0x00000000**
- Bit 16 STATE = 0 → **HFCLK NOT running**
- Bit 0  SRC   = 0 → moot when STATE=0

**HFCLKRUN = 0** — no HFCLKSTART request pending. Consistent.

**LFCLKSTAT = 0x00010001** — LFCLK running, source = Xtal (32.768 kHz).
Expected.

**DHCSR = 0x00050001**
- Bit 0  C_DEBUGEN = 1  → SWD attached, halting-debug enabled
- Bit 16 S_REGRDY  = 1  → last register access completed
- Bit 17 S_HALT    = 0  → **CPU NOT halted**
- Bit 18 S_SLEEP   = 1  → **CPU is in WFE/WFI (idle)**

So: the CPU IS in sleep mode, the SoC's system power domain IS up
(due to CDBGPWRUPREQ from OpenOCD), and **HFCLK is genuinely OFF**
in the settled state — repeatable across 5 samples over ~5.5 s.

## What this inverts

The target-side `APP_HW_HFCLK_PROBE` reads HFCLKSTAT from inside a
LOG_INF path. That path runs on the CPU. The CPU running requires
HFCLK to be on. So every target-side read shows STATE=1 — trivially,
and not because HFCLK stays on continuously.

The SWD-side MEM-AP read is a bus-master access that does NOT wake
the CPU. It observes HFCLKSTAT while the CPU is genuinely in WFE.
Under those conditions HFCLK reads STATE=0.

**Conclusion: the firmware IS reaching HFCLK-off System-ON idle
between wakes.** The 1.65 mA floor we've been chasing is not a
firmware anchor. Every HFCLK-anchor hypothesis from the prior 3
working docs (chatty log, turbo poll, USB stack, TIMER2 counter, PM
subsystem, MPSL, HPTIMER, generic TIMER instance) was chasing a
symptom that doesn't exist.

## What's actually drawing the 1.65 mA

Strongly suspected: **SWD-attach current draw itself**. On nRF52,
having `CDBGPWRUPREQ` asserted keeps the debug interface + DAP
powered up. Nordic's own docs put this at ~1-2 mA additional current
depending on chip revision, which matches our 1.65 mA baseline almost
exactly.

This means:

- The firmware is fine. Sleepy ED behavior is working correctly.
- Every INA219 capture on the rig with SWD wired has ~1.5 mA of
  overhead that has nothing to do with the firmware.
- The true "battery-life" current is what remains AFTER SWD is
  physically disconnected.

## Limitation of SWD-side diagnosis

We can't test the SWD-detached case via SWD — the tool being tested
is the tool doing the observing. Every OpenOCD `init` asserts
CDBGPWRUPREQ, which is precisely the mechanism suspected of anchoring
the current. The only definitive test is physical disconnect.

## Physical-disconnect procedure

To confirm this interpretation and get the true firmware idle current:

1. **Confirm no OpenOCD is running on rpi-xiao**:
   ```
   ssh rpi-xiao 'pgrep openocd || echo idle'
   ```
   Should print `idle`.

2. **Start INA219 sampling as a baseline** (SWD still attached,
   for the "before" number):
   ```
   tools/ina219-sample.sh swd-attached-before 60 10
   ```
   Expected mean ~1.65 mA.

3. **Physically pull the four SWD jumpers from the XIAO's SWD pads**:
   - SWCLK
   - SWDIO
   - GND (only if it's a dedicated SWD ground; if it's the shared
     ground for INA219 power path, LEAVE IT — don't break the
     power circuit)
   - VTREF / 3V3 reference (if wired)

   Do this cleanly — don't pinch the SWD lines against another
   contact or you'll re-assert them differently.

4. **Power-cycle the XIAO** to clear any latched debug state:
   Briefly break the Pi 3V3 → BAT wire (or pull it out and re-seat).
   The XIAO reboots, comes up without any prior debug attach.

5. **Restart INA219 sampling for the "after" measurement**:
   ```
   tools/ina219-sample.sh swd-detached-after 180 10
   ```
   Wait at least 60 s for a fresh join + settle.

6. **Interpret**:
   - **Mean < 100 µA**: SWD-attach was the anchor. Firmware is
     already at sub-mA idle. Close the anchor thread. Move to PPK2
     (or same INA219 rig with SWD physically disconnected) for
     any future battery-life measurement.
   - **Mean stays ~1.65 mA**: SWD isn't the anchor. Something else
     is drawing ~1.5 mA that we haven't identified. Extremely
     unlikely given the SWD-side evidence, but not ruled out until
     measured.

## Landing decision

Land this doc + memory update. Do NOT land any firmware change yet —
the firmware isn't the problem. Next physical session runs the
disconnect test above.

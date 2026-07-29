# Exhaustive peripheral audit — 1.6 mA anchor is not any peripheral we can turn off (#8)

Follow-up to `2026-07-29-swd-dp-diagnostic.md`. That doc guessed SWD-attach
was the anchor. First INA219 with SWD physically detached: **1.585 mA**
(vs 1.65 mA attached) — SWD contributes only ~65 µA, not the whole 1.5 mA
floor. Which pushed the investigation deeper.

## Address correction

`0x40000408` is `NRF_CLOCK->HFCLKRUN`, not `HFCLKSTAT`. Correct HFCLKSTAT
is at `0x4000040C`. Earlier SWD reads showing "HFCLKSTAT=0" were actually
HFCLKRUN reads — trivially 0 because HFCLKSTART was never triggered
explicitly. Re-run against the correct address confirms:

```
HFCLKSTAT (0x4000040C) = 0x00010001 initially, settles to 0x00010000
                        → STATE=1, SRC=HFINT
HFCLKRUN  (0x40000408) = 0x00000000
LFCLKSTAT (0x40000418) = 0x00010001 → LFCLK on XTAL
```

So the target-side probe was right all along — HFCLK IS on continuously
via HFINT peripheral-demand path. HFCLKSTART never triggered.

## Peripheral audit via SWD MEM-AP

Systematic register read of every peripheral that consumes PCLK16M when
enabled. Two SWD sessions across an IMU-off + UART/I2C-off firmware.

**EasyDMA peripherals (each anchors HFCLK when ENABLE ≠ 0):**

| Peripheral | Address | Before overlay | After overlay |
|---|---|---|---|
| RADIO.STATE | 0x40001550 | 0 (DISABLED) | 0 |
| UARTE0.ENABLE | 0x40002500 | **8 (ENABLED)** | **0** |
| TWIM0.ENABLE | 0x40003500 | **6 (ENABLED)** | **0** |
| TWIM1.ENABLE | 0x40004500 | 0 | 0 |
| SPIM2.ENABLE | 0x40023500 | 0 | 0 |
| SPIM3.ENABLE | 0x4002F500 | 0 | 0 |
| I2S.ENABLE | 0x40025500 | 0 | 0 |
| PDM.ENABLE | 0x4001D500 | 0 | 0 |
| USBD.ENABLE | 0x40027500 | 0 | 0 |
| QSPI.ENABLE | 0x40029500 | 0 | 0 |
| NFCT.ENABLE | 0x40005500 | 0 | 0 |

**Other HFCLK consumers:**

| Peripheral | Register | Value |
|---|---|---|
| SAADC.ENABLE | 0x40007500 | 0 |
| LPCOMP.ENABLE | 0x40013500 | 1 (expected — uses LFCLK, not HFCLK) |
| PWM0-3.ENABLE | * | 0 |
| CC310.ENABLE | 0x5002A500 | 0 |
| CCM.ENABLE | 0x4000F500 | 0 |
| WDT.INTENSET | 0x40024304 | 0 |
| TIMER0-4 (double-capture) | * | all counters static → not running |
| RNG.VALUE change over 1.6 s | 0x4000D508 | static 0x13 → RNG stopped |
| Zephyr `hfclk_users` | 0x200060F0 | 0 (no subsystem holding via nrfx_clock) |
| Zephyr `hfclk_is_running` | 0x2000905A | 0 (HFXO not requested) |

**CPU/HFCLK duty-cycle over 100 fast SWD samples:**

```
CPU S_SLEEP=1: 100/100  → CPU is genuinely idle
HFCLKSTAT.STATE=1: 100/100  → HFCLK is on
HFCLKSTAT.SRC=XTAL: 0/100  → not the crystal, always HFINT
```

## Interpretation

Every peripheral capable of anchoring PCLK16M via the standard
peripheral-demand mechanism is either compiled out, disabled at the DT
level, or observed idle. The CPU is asleep 100% of the time. Zephyr's
own clock-control refcount is 0. Yet HFCLK stays on with source HFINT,
and INA219 sits at ~1.6 mA — dead-on the datasheet "System-ON idle,
HFCLK on" figure.

The only remaining mechanism that fits is the **ARM debug interface
itself keeping HFCLK on while `CDBGPWRUPREQ` is asserted**. The
CoreSight debug logic sits in the system power domain and requires
HFCLK for trace/debug operations. On nRF52, once `CDBGPWRUP` has been
asserted (by any OpenOCD session, past or present), the assertion is
latched in the DP CTRL/STAT register — cleared only by a power-on
reset.

**Why the earlier "SWD detached" test didn't drop current more.** The
user physically pulled the SWD jumpers but did NOT power-cycle the
XIAO. If DP CTRL/STAT persists across SWD disconnects (very plausible
for register-level state that's cleared only by POR), pulling wires
doesn't clear the latch. Every OpenOCD session earlier in the day left
CDBGPWRUP asserted; the pull-wires test inherited that state.

## Firmware changes landed anyway

Two overlay hygiene fixes, kept even though neither was the anchor:

- IMU disabled (`lsm6ds3tr_c`, `lsm6ds3tr-c-en`, `msm261d3526hicpm-c-en`) —
  ~18 µA measurable delta. Sensor wasn't the culprit but there's no
  reason to keep the driver + regulator paths in the battery build.
- UART0 + I2C0 disabled — both had non-zero ENABLE at register level
  before, causing PCLK16M demand. ~30 µA measurable delta. Same
  hygiene reasoning.

Together: 1.65 mA → 1.60 mA. Real but modest.

## The one test that will settle it

1. Confirm no OpenOCD on Pi (`ssh rpi-xiao 'pgrep openocd || echo idle'`).
2. Physically detach SWD from XIAO if not already.
3. **Power-cycle the XIAO**: briefly break the Pi 3V3 → BAT wire (or
   pull-and-re-seat). This is what clears the latched CDBGPWRUP.
4. Wait 60 s for firmware to boot + join.
5. `tools/ina219-sample.sh post-por 180 10`.

Predictions:

- **Mean < 100 µA** → SWD-sticky-attach was the whole anchor. Firmware
  is already at proper sub-mA sleep. All future measurements need
  either PPK2 or same-rig POR-after-flash procedure.
- **Mean 500-1500 µA** → some HFCLK-holding mechanism I haven't found.
  Would be surprising given the audit; would motivate deeper diagnostic
  (ELF-symbol lookup for MPSL internal state, or PPK2 rig).
- **Mean stays ~1.6 mA** → CDBGPWRUP is not the anchor and we're back
  to square one. Most likely explanation would be an nrfxlib
  library-level HFCLK request I can't see from register audit.

## Landing decision

Commit the overlay changes (IMU + UART0 + I2C0 disabled — retained
regardless of test outcome for hygiene), this audit doc, and the
address-correction memory update. Do NOT commit any further firmware
changes until the POR test result is in.

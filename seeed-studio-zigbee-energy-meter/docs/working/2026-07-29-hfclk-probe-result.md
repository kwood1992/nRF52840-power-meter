# HFCLK probe result — HFCLK is on continuously, source=HFINT, no explicit request (#8)

Follow-up to `2026-07-29-hfclk-anchor-hunt.md`. That doc left the
residual 1.65 mA pointing at HFCLK being held on somewhere below the
app but couldn't tell what was actually happening at the peripheral
status registers. New `APP_HW_HFCLK_PROBE` Kconfig + `rtt.conf` build
answers that question directly.

## What was added

- New Kconfig `APP_HW_HFCLK_PROBE` that logs `NRF_CLOCK->HFCLKSTAT`,
  `NRF_CLOCK->HFCLKRUN`, and `nrf_clock_hf_is_running(NRF_CLOCK,
  NRF_CLOCK_HFCLK_HIGH_ACCURACY)` from the same tick+kick call sites
  the rx-idle probe already owns (boot, post-steering,
  post-reboot-reattach, 60 s tick). `APP_HW_HFCLK_PROBE`
  `select`s `APP_ZIGBEE_RX_IDLE_PROBE` so the shared 60 s work item
  fires. Both flipped on by `rtt.conf`.

## Capture protocol

- `FLASH_METHOD=swd tools/test-join.sh` — clean factory-reset + join
  cycle on the off-USB rig. PASS (interview_state=SUCCESSFUL).
- `tools/rtt-tail.sh hfclk-probe` — starts fresh SWD-RTT session.
  OpenOCD reset-run causes the device to re-attach on saved network
  (`ZB_BDB_SIGNAL_DEVICE_REBOOT`) instead of steering — same joined
  state, no factory reset needed for the probe.
- `tools/ina219-sample.sh hfclk-probe 180 10` — 180 s parallel INA219
  capture on rpi-xiao during the RTT window.

Artifacts:
- `docs/working/rtt-2026-07-29-154746-hfclk-probe.log`
- `docs/working/ina219-2026-07-29-154845-hfclk-probe.csv`

## Result — all seven probe reads identical

| where                      | HFCLKSTAT   | HFCLKRUN    | xtal_running |
|----------------------------|-------------|-------------|--------------|
| post-configure-sleepy      | 0x00010000  | 0x00000000  | 0            |
| post-reboot-reattach       | 0x00010000  | 0x00000000  | 0            |
| tick @ 1 min post-boot     | 0x00010000  | 0x00000000  | 0            |
| tick @ 2 min post-boot     | 0x00010000  | 0x00000000  | 0            |
| tick @ 3 min post-boot     | 0x00010000  | 0x00000000  | 0            |
| tick @ 4 min post-boot     | 0x00010000  | 0x00000000  | 0            |
| tick @ 5 min post-boot     | 0x00010000  | 0x00000000  | 0            |

`HFCLKSTAT=0x00010000` decodes as:

- `STATE` bit (16): `1` — HFCLK is running.
- `SRC`   bit (0):  `0` — source is HFINT (internal 16 MHz RC), NOT the
  external crystal (XO/HFXO).

`HFCLKRUN=0` means no `TASKS_HFCLKSTART` has been fired and left
outstanding. Nobody has explicitly requested the HFCLK-with-crystal.

`xtal_running=0` corroborates: the HFXO is not running, so MPSL /
nrf_802154 have not asked for the high-accuracy clock either.

INA219 for the same window: n=1801, mean=1.886 mA, sd=0.793 mA,
min=1.400 mA, p50=1.900 mA, p95=2.100 mA, max=18.600 mA. The variance
is dominated by long-poll TX bursts to the parent every 60 s; the
floor matches the datasheet's "CPU System-ON idle, HFCLK on" figure
(~1.6 mA).

## Interpretation

HFCLK is on continuously, running on HFINT (fast-start, ~40 µs to lock,
no crystal warm-up). It was NOT started by application code, MPSL, or
the 802.15.4 driver — those paths all go through `HFCLKSTART` and
request the crystal, which would show `HFCLKRUN=1` and
`xtal_running=1`. Something is anchoring HFCLK by *implicit peripheral
demand* — a peripheral that consumes PCLK16M autostarts HFCLK on
HFINT, and won't let it stop as long as the peripheral is armed.

**Caveat about the probe itself.** The reads happen inside a work-item
handler that runs LOG_INF, so the CPU is by definition active at the
moment of reading. HFCLK naturally is on then. What the reads
genuinely prove is:

- source is HFINT, not XO — no Zigbee-stack HFCLKSTART is pending
- HFCLKRUN=0 — no explicit-request path is holding HFCLK

For the "does HFCLK ever drop between wakes" question, the INA219 min
of 1.4 mA (rare) vs the p50 of 1.9 mA (typical) suggests HFCLK does
drop briefly but the floor is dominated by HFCLK-on periods. We're
not seeing System-ON deep-sleep floor levels (single-digit µA).

## Candidates for the peripheral anchor

Priority order for the next investigation:

1. **Zephyr system-tick timer source.** NCS default on nRF52 is
   `CONFIG_NRF_RTC_TIMER=y` (RTC1, LFCLK-based — should NOT anchor
   HFCLK). If the config has silently switched to `CONFIG_NRF_TIMER`
   or a similar HFCLK-based tick, that's the anchor. Verify in the
   generated `zephyr/.config`.

2. **`NRF_802154_SL_HPTIMER`.** `def_bool y` on `SOC_SERIES_NRF52X` per
   the earlier hunt doc. If armed continuously for CCA/backoff/IFS
   timing, keeps HFCLK on. Toggle-able? Check the SL Kconfig tree.

3. **ZBOSS scheduler timer.** `CONFIG_ZIGBEE_TIME_KTIMER=y` (Zephyr-
   timer-backed) — should route through Zephyr's tick and inherit
   the tick's clock choice. If tick is on RTC (case 1 correct),
   this is a non-issue. If tick is on TIMER, this piles on.

4. **Any EasyDMA peripheral left initialised at boot.** LPCOMP, UART
   (UART_CONSOLE=n in rtt.conf but the CDC-ACM stack disable is
   already done), SPI/TWI (unused here), I2S — walk `zephyr/.config`
   for enabled peripherals and cross-check the HFCLK-consumer list.

## Config-level check ruled out candidate #1

30-s grep of `build/zephyr/.config`:

```
CONFIG_NRF_RTC_TIMER=y                  # Zephyr tick on RTC (LFCLK), not TIMER
CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=32768
CONFIG_ZIGBEE_TIME_KTIMER=y             # ZBOSS timer → Zephyr tick → RTC
CONFIG_MPSL=y
CONFIG_MPSL_HFCLK_LATENCY=1400          # MPSL asks for HFXO 1400 µs before slots
CONFIG_MPSL_TIMESLOT_SESSION_COUNT=0    # no BLE/Thread coexistence slots
CONFIG_NRF_802154_SL_HPTIMER=y          # HFCLK-based precision timer
```

Candidate #1 (Zephyr tick on TIMER) is definitively out — tick source
is `NRF_RTC_TIMER`, running from LFCLK. Rules out ZBOSS scheduler as
well since `ZIGBEE_TIME_KTIMER=y` piggy-backs on the same tick.

Remaining live candidates are **MPSL** and **NRF_802154_SL_HPTIMER**.
Note the paradox — both of those *would* set `HFCLKRUN=1` if they
were holding HFXO. `HFCLKRUN=0` and `xtal_running=0` say neither is.
Either MPSL/802.15.4 aren't the anchor after all (and something more
mundane, like an EasyDMA peripheral, is), OR they're anchoring HFCLK
via peripheral-level clock demand (HPTIMER's TIMER instance) rather
than via the HFCLKSTART API.

## Next actions (not done in this session)

- Extend the probe to read TIMER0-4 `INTENSET` / `MODE` registers —
  if any TIMER is initialised in Timer mode (not Counter), it consumes
  PCLK16M and anchors HFCLK. HPTIMER's TIMER instance would show up
  here. This is the single next-lever most likely to point at the
  culprit.
- Check `mpsl_clock_hfclk_is_running()` (if the API is exposed on this
  NCS build) to verify MPSL's own view of HFCLK ownership.
- If HPTIMER turns out to be the anchor, evaluate `CONFIG_NRF_802154_SL_HPTIMER=n`
  — cost is degraded CCA/backoff timing precision; benefit is possibly
  a large drop in idle current. Only worth doing if we're deliberately
  chasing sub-mA and can accept the timing tradeoff.

## Landing decision

Land the probe infra + this result doc. The Kconfig gate + the shared
tick with rx-idle probe means turning the probe back on for a follow-up
is a one-line change in rtt.conf. The 1.65 mA baseline stays where it
was (~30 days on 2×AAA lithium); pushing sub-mA is a separate ticket.

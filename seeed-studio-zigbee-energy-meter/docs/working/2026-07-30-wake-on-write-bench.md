# Short-press wake-on-write — bench verification (#62, closes #50)

Date: 2026-07-30
Firmware: `feat/62-short-press-wake-on-write` (commit `7721a3c`)
Rig: XIAO on Pi 3V3 → BAT via BCM 22 relay, USB unplugged, SWD attached,
flashed via `tools/flash-swd.sh`.

## What was tested

A controlled A/B on the same attribute write, differing only in whether a
short button press preceded it. Both arms ran with the device in genuine
steady state — 110 s after join, so the 30 s post-join turbo-poll window
had definitively expired and the 60 s long poll was in effect.

Write used the **friendly form** `{"imp_per_kwh": 900}`, which the external
converter translates to `seMetering.write({"770":{"value":900,"type":34}})`.
That is the exact path #50 needed exercised directly.

## Control — no button press

```
02:46:07  z2m-cli pub 0xf4ce361b0656e80e/set {"imp_per_kwh": 900}
```

```
{"level":"error","message":"z2m: Publish 'set' 'imp_per_kwh' to
'0xf4ce361b0656e80e' failed: 'Error: ZCL command
0xf4ce361b0656e80e/10 seMetering.write({\"770\":{\"value\":900,\"type\":34}},
{\"timeout\":10000, ...}) failed
(Timeout after 10000ms [address=32194 endpoint=10 clusterId=1794 cmdId=4 tsn=24])'"}
```

Reproduces #62's premise precisely: `Timeout after 10000ms`.

## Treatment — short press, then the same write

```
02:46:43  xiao-short-press.sh   (250 ms → classifies SHORT, < 1000 ms)
02:46:45  z2m-cli pub 0xf4ce361b0656e80e/set {"imp_per_kwh": 900}
```

```
{"energy":3504.04,"imp_per_kwh":900,"linkquality":72,"min_pulse_width_us":2500,"power":0}
{"energy":3504.04,"imp_per_kwh":900,"linkquality":75,"min_pulse_width_us":2500,"power":0}
```

Write landed. Total error count in the Z2M log across the whole session
stayed at **1** — the control arm's timeout, with none from the treatment.

Repeated a second time at the end of the session to restore the rig's real
calibration (`{"imp_per_kwh": 800}`) — also succeeded. Treatment n=2, both
successes; control n=1, timeout.

## Join state undisturbed

The explicit AC. Across the whole subscriber window (opened before the
control write, covering both presses):

- `device_joined` / `device_leave` / `device_interview` / `device_announce`
  events referencing our IEEE: **0**
- `bridge/health` for our device: `leave_count=2` (both from
  `test-join.sh`'s own factory resets, before the experiment),
  `network_address_changes=0`

So the press changed poll cadence only — no re-steer, no re-interview.

## Persistence across reboot (closes #50's last by-proxy AC)

Single RST pulse (GPIO 23), 40 s for boot + re-attach, then a read:

```
{"energy":3504.04,"imp_per_kwh":900,"linkquality":81,"min_pulse_width_us":2500,"power":0}
```

`imp_per_kwh` held at the written 900 across a cold reload from NVS, and
`energy` held at 3504.04 — the accumulator persisted too.

## Sleep-current AC — argued, not measured

#62's last checkbox asks that the window's sleep cost be bounded and not
affect steady state. This is satisfied by construction rather than
measurement: `zb_zdo_pim_start_turbo_poll_continuous` runs only when the
user presses the button, and ZBOSS reverts to the long-poll interval when
the window expires. So the steady-state delta is exactly zero, and the
transient is bounded at 30 s of ~100 ms polling
(`CONFIG_APP_ZIGBEE_WAKE_TURBO_POLL_MS`).

Worth stating plainly: an INA219 run would **not** add information here.
Per #58, that rig has ±300 µA single-sample noise and cannot resolve below
~50 µA, and a bounded 30 s user-triggered transient is not what the
battery-life projection is sensitive to.

## Rig state left behind

- Device joined, interview SUCCESSFUL, `imp_per_kwh` restored to **800**.
- `permit_join` off. All bench MQTT subscribers stopped.
- No openocd left running (see the note below).

## Gotcha worth remembering

`tools/rtt-tail.sh` leaves a **root-owned openocd running indefinitely**,
holding the SWD bus. It cost this session a failed flash: `flash-swd.sh`
spawned a second openocd, the two fought over SWCLK/SWDIO, and the erase
failed partway (`Wrong parity detected`, `Error waiting NVMC_READY`,
`failed erasing sectors 39 to 121`) leaving the app slot partially erased.
The tell is `couldn't bind gdb to socket on port 3333: Address already in use`.

Compounding it: running the `pinctrl set 24 ip pd` continuity probe from
the SWD-recovery notes **while** openocd owns the pins wedges that instance
into `Error connecting DP: cannot read IDR` permanently — its bitbang driver
sets pin direction once at init and never re-asserts it. A POR does not
recover this; only restarting openocd does.

Check before any SWD flash:

```
ssh rpi-xiao 'ps -eo pid,user,etime,cmd | grep [o]penocd'
```

Clearing it needs the user — Pi passwordless sudo is openocd-only, so
`sudo pkill -f openocd` must be run interactively.

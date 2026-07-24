# SWD recovery + one-command flash jig

Firmware iteration on the XIAO nRF52840 is painful without a way to reset
the board without pressing its tiny top-side reset button. This directory
documents a **Raspberry Pi + 3D-printed spring-pin jig** setup that turns
the flash-and-reset loop into a single Mac command:

```
./tools/flash.sh
```

The same setup also **recovers a soft-bricked XIAO** — the situation
where a bad firmware overwrites the Adafruit UF2 bootloader region and
double-tap-reset stops working. See "Emergency recovery" at the end.

---

## What you need

- **Raspberry Pi 3B+ (or newer, non-Pi 5)** on your local network with
  SSH access. Pi 4 works identically. Pi 5 needs a different OpenOCD
  interface config (`raspberrypi5-gpiod.cfg`) — everything else is the
  same.
- **Female-female jumper wires** or a set of pogo pins mounted in a jig.
- **3D-printed spring-pin jig** — see next section. Optional but hugely
  reduces the pain of clipping onto the XIAO's tiny back-side pads.
- The XIAO plugged into your Mac via USB (powers the board — the Pi
  provides SWD signals only, not power).

---

## The 3D-printed jig

A jig that holds spring-loaded pogo pins against the XIAO's five
underside SWD pads (SWDIO, SWCLK, RST, 3V3, GND) so you don't need to
solder or hand-hold probes. Prints in ~1 hour on a Bambu.

**MakerWorld link**: *TODO — placeholder until upload*

`https://makerworld.com/en/models/PLACEHOLDER-xiao-nrf52840-swd-jig`

BOM for the jig:
- 5 × spring-pin (pogo pin) contacts, 0.68 mm tip diameter, ~4 mm travel
- 5 × female Dupont jumper wires
- Two tiny M2 screws (or press-fit if your printer's tolerances are good)

---

## Wiring

XIAO underside pads (marked on the PCB silkscreen; also documented in
Seeed's *Bottom-pad-positioning* ZIP):

| XIAO pad | Pi header pin | Pi BCM GPIO | Purpose |
|---|---|---|---|
| SWDIO   | 18 | GPIO 24 | Serial Wire Data I/O |
| SWCLK   | 22 | GPIO 25 | Serial Wire Clock |
| RST     | 16 | GPIO 23 | Hardware reset (also used to emulate double-tap) |
| GND     | 6  | —       | Common ground (mandatory) |
| 3V3     | —  | —       | **DO NOT WIRE**. XIAO gets power from USB. Back-feeding 3V3 from Pi damages regulators. |

Physical layout of the Pi 40-pin header is documented at
<https://pinout.xyz>. Pin 1 is the corner nearest the SD card slot;
count down the columns to find 6, 16, 18, 22.

### Optional bench-input wires (front-of-XIAO pins)

Two extra jumpers let the Pi drive on-board interrupts over SSH so you
can exercise the firmware without touching the board. Both are
active-low inputs with internal pull-ups; the Pi pulls them LOW to fire
an edge.

| XIAO pin | Pi header pin | Pi BCM GPIO | Purpose |
|---|---|---|---|
| D6 (P1.11) | 11 | GPIO 17 | User button — `~/xiao-short-press.sh` joins, `~/xiao-long-press.sh` factory-resets |
| D7 (P1.12) | 13 | GPIO 27 | Bench pulse-simulator — `~/xiao-pulse.sh` bumps the accumulator by 1 with no join-callback side effect (see issue #16) |

`GND` is already wired through the SWD block above, so the input wires
only need one line each. The two `~/xiao-pulse*` helper scripts and the
`~/xiao-short/long-press.sh` scripts live on the Pi; templates are in
[`tools/`](../tools/) — copy them into the Pi's home dir and `chmod +x`.

---

## Software setup (one-time)

### On the Pi

Install OpenOCD (usually already present on Bookworm):

```
sudo apt-get install openocd
```

Give the user passwordless sudo for openocd (needed for GPIO register
access):

```
echo 'ctadmin ALL=(root) NOPASSWD: /usr/bin/openocd' | sudo tee /etc/sudoers.d/openocd-nopasswd
sudo chmod 440 /etc/sudoers.d/openocd-nopasswd
```

Install the double-tap-reset helper script at `~/xiao-bootloader.sh`:

```bash
cat > ~/xiao-bootloader.sh <<'EOF'
#!/bin/bash
# Emulate double-tap reset to force XIAO into UF2 bootloader mode.
pinctrl set 23 op dh
sleep 0.05
pinctrl set 23 dl
sleep 0.05
pinctrl set 23 dh
sleep 0.25
pinctrl set 23 dl
sleep 0.05
pinctrl set 23 dh
sleep 0.05
pinctrl set 23 ip pu
EOF
chmod +x ~/xiao-bootloader.sh
```

### On the Mac

Generate an ed25519 key without a passphrase and add it to the Pi's
`~/.ssh/authorized_keys`. Add the following to `~/.ssh/config`:

```
Host rpi-xiao
    HostName <your-pi-ip>
    User <your-pi-user>
    IdentityFile ~/.ssh/xiao-recovery-rpi
    IdentitiesOnly yes
```

Verify with `ssh rpi-xiao uptime` — should return the Pi's uptime
without prompting for a password.

---

## Daily use — one-command flash

After any `west build`, from the repo root:

```
./tools/flash.sh
```

That's it. The script:

1. SSHes to the Pi and pulses GPIO 23 twice (~300 ms apart) to
   emulate the Adafruit UF2 bootloader's double-tap-reset detection.
2. Waits up to 15 s for `/Volumes/XIAO-SENSE` to mount.
3. Copies `seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.uf2`
   to the drive. The bootloader accepts it, writes it to flash, and
   reboots into the new firmware.
4. Confirms success by waiting for the drive to unmount.

To flash a specific UF2 (not the standard build output):

```
./tools/flash.sh path/to/other.uf2
```

---

## Emergency recovery — soft-bricked board

If a bad firmware overwrites the bootloader (`pm_static.yml`
misconfigured, mass-erase that included UICR, etc.) the XIAO becomes
completely dark — no LED, no USB enumeration, no response to
double-tap. The jig recovers it via SWD:

### Symptoms

- Board plugged in, nothing shows on `ls /dev/tty.usbmodem*`
- No `XIAO-SENSE` drive mounts on double-tap
- LED never blinks

### Recovery

1. **Confirm the SoC is alive over SWD**:

   ```
   ssh rpi-xiao 'sudo openocd -f interface/raspberrypi-swd.cfg \
     -c "adapter speed 500" -c "transport select swd" \
     -f target/nordic/nrf52.cfg -c "init; targets; exit"'
   ```

   Expect `SWD DPIDR 0x2ba01477 DPv1` and `Cortex-M4 r0p1 processor
   detected`. If not, check wiring and try again.

2. **Download the Adafruit UF2 bootloader hex** to the Pi:

   ```
   ssh rpi-xiao 'curl -sL -o /tmp/xiao-sense-bootloader.hex \
     https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases/download/0.11.0/xiao_nrf52840_ble_sense_bootloader-0.11.0_s140_7.3.0.hex'
   ```

   *(For the plain XIAO nRF52840 without the Sense IMU, use
   `xiao_nrf52840_ble_bootloader-0.11.0_s140_7.3.0.hex` instead.)*

3. **Mass-erase + reflash the bootloader** via OpenOCD:

   ```
   ssh rpi-xiao 'sudo timeout 240 openocd \
     -f interface/raspberrypi-swd.cfg \
     -c "adapter speed 500" -c "transport select swd" \
     -f target/nordic/nrf52.cfg \
     -c "init" -c "halt" -c "nrf5 mass_erase" \
     -c "program /tmp/xiao-sense-bootloader.hex verify" \
     -c "reset run" -c "exit"'
   ```

   Expect `Mass erase completed`, then `** Verified OK **`, then reset.

4. **Confirm recovery**: unplug the XIAO from USB, plug it back in. On
   its own it enters DFU mode (no firmware installed post-mass-erase);
   `XIAO-SENSE` should mount. From here, drag any UF2 to flash a new
   firmware.

---

## Why this exists

We soft-bricked the first XIAO on 2026-07-22 while iterating on the
ncs-zigbee R23 integration: `pm_static.yml` placed `zboss_nvram` at
0xf4000, the exact flash region the Adafruit UF2 bootloader occupies.
The flash-write erased the bootloader; the board went dark. Recovery
via a Pi + hand-held jumper wires took ~30 minutes of careful poking.
The jig + `tools/flash.sh` cuts that down to seconds and — importantly
— makes the recovery path something you can rely on rather than a
one-off scramble.

Related project memory:

- `project_xiao_flash_reserved_regions.md` — which flash addresses are
  bootloader territory and must never be written by the app
- `project_reset_on_fatal_masks_bootloader.md` — why
  `CONFIG_RESET_ON_FATAL_ERROR` masks double-tap recovery during first-
  time integration
- `reference_swd_flash_workflow.md` — the flash.sh workflow itself

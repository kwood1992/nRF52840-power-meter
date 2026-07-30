# SWD: recovering a bricked board, and the one-command flash rig

Two things live here:

1. **[Emergency recovery](#emergency-recovery--soft-bricked-board)** — how to
   revive a XIAO whose bootloader got overwritten. Works with any common SWD
   probe. **Read this first if your board has gone dark.**
2. **[The Raspberry Pi flash rig](#optional-the-raspberry-pi-flash-rig)** —
   an optional bench setup that turns "double-tap reset, wait for the drive,
   drag the UF2" into `./tools/flash.sh`. Nice to have, not required.

You do **not** need any of this for normal use. Flashing a XIAO is UF2 over
USB with no debugger. SWD is the safety net for when that stops working.

---

## Do you actually need SWD?

Work down this list before wiring anything.

| Symptom | Try this first |
|---|---|
| No `XIAO-SENSE` drive on double-tap | **Is your screen locked?** macOS Sequoia won't mount the volume while locked. Unlock and retry — this looks exactly like a dead bootloader. |
| Drive mounts but firmware never runs | Almost always the sysbuild linker bug, not a brick. See [CONTRIBUTING → Build gotchas](../CONTRIBUTING.md#build-gotchas). |
| Board enumerates but the app is faulting | Double-tap still works. Reflash a known-good UF2. |
| Nothing at all: no LED, no USB device, no drive, no response to double-tap | **Genuine brick.** Continue below. |

A genuine brick means the flash region holding the Adafruit UF2 bootloader
(`0xF4000`–`0x100000`) was erased or overwritten. Nothing but SWD gets it
back — there is no software path left on the board to talk to.

---

## Emergency recovery — soft-bricked board

### 1. Pick a probe

Any SWD probe works. In rough order of "what people already own":

| Probe | Host tool | Notes |
|---|---|---|
| **Raspberry Pi** (3B+/4, GPIO bit-bang) | OpenOCD | No extra hardware if you have a Pi. Pi 5 needs `raspberrypi5-gpiod.cfg` instead. |
| **Raspberry Pi Pico** as [picoprobe/debugprobe](https://github.com/raspberrypi/debugprobe) | OpenOCD or pyOCD | ~$5. Flash the debugprobe UF2 onto the Pico and it becomes a CMSIS-DAP probe. |
| **SEGGER J-Link** (incl. EDU) | `nrfjprog`, OpenOCD, or J-Link Commander | Nordic's first-class path. |
| **ST-Link v2** (clone is fine) | OpenOCD or pyOCD | Cheap and everywhere. |
| Any **CMSIS-DAP** adapter | pyOCD, probe-rs, OpenOCD | Generic fallback. |

### 2. Wire it

Five pads on the **underside** of the XIAO. Only three signals plus ground
are needed.

| XIAO pad | Connect to | Required? |
|---|---|---|
| SWDIO | Probe SWDIO / SWD data | Yes |
| SWCLK | Probe SWCLK / SWD clock | Yes |
| GND | Probe GND | **Yes — mandatory** |
| RST | Probe reset (or a GPIO) | Optional; helps when the target won't halt |
| 3V3 | **Leave disconnected** if the board has USB or battery power | Only if the board is otherwise unpowered |

> **Do not back-feed 3V3 while the XIAO is powered from USB or a battery.**
> Two supplies fighting through different regulators can damage them. Power
> the board from exactly one source.

The pads are silkscreened; Seeed also publishes a *Bottom-pad-positioning*
drawing with exact coordinates. They are small — a
[3D-printed pogo-pin jig](#the-3d-printed-jig) makes this much less painful
if you'll be doing it more than once, but hand-held jumpers work for a
one-off rescue.

**Raspberry Pi GPIO wiring** (BCM numbering; physical header positions per
<https://pinout.xyz>):

| XIAO pad | Pi header pin | Pi BCM |
|---|---|---|
| SWDIO | 18 | GPIO 24 |
| SWCLK | 22 | GPIO 25 |
| RST | 16 | GPIO 23 |
| GND | 6 | — |

### 3. Confirm the SoC is alive

Before assuming the worst, check the chip still answers on SWD. A bricked
bootloader does **not** mean a dead SoC — the debug port is in silicon and
responds regardless of what's in flash.

With OpenOCD on a Raspberry Pi:

```bash
sudo openocd -f interface/raspberrypi-swd.cfg \
  -c "adapter speed 500" -c "transport select swd" \
  -f target/nordic/nrf52.cfg -c "init; targets; exit"
```

With a CMSIS-DAP / picoprobe / ST-Link, swap the interface file:

```bash
openocd -f interface/cmsis-dap.cfg \
  -c "adapter speed 500" -c "transport select swd" \
  -f target/nordic/nrf52.cfg -c "init; targets; exit"
```

Expect `SWD DPIDR 0x2ba01477` and `Cortex-M4 r0p1 processor detected`. If
you get nothing, it's wiring — check GND continuity first, then that you
haven't swapped SWDIO and SWCLK.

> **If the chip is readable but flash operations fail with parity or NVMC
> errors, check for a second OpenOCD.** Two OpenOCD processes on the same
> wires will corrupt an erase midway and leave the app slot partially
> written. On the Pi rig, `./tools/rtt-tail.sh --stop` clears the usual
> culprit. See [tools/README](../tools/README.md#if-a-flash-fails-with-parity--nvmc-errors-check-for-a-leaked-openocd).

### 4. Get the right bootloader image

Match the variant — Sense and plain have different images.

```bash
# Sense
curl -sL -o xiao-bootloader.hex \
  https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases/download/0.11.0/xiao_nrf52840_ble_sense_bootloader-0.11.0_s140_7.3.0.hex

# Plain (non-Sense)
curl -sL -o xiao-bootloader.hex \
  https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases/download/0.11.0/xiao_nrf52840_ble_bootloader-0.11.0_s140_7.3.0.hex
```

Check the [releases page](https://github.com/adafruit/Adafruit_nRF52_Bootloader/releases)
for a newer version; the filenames follow the same pattern.

### 5. Mass-erase and reflash

The mass erase is what makes this work — it clears the UICR alongside
flash, which a plain program does not.

**OpenOCD** (any probe; change the `interface/` file to match yours):

```bash
sudo timeout 240 openocd \
  -f interface/raspberrypi-swd.cfg \
  -c "adapter speed 500" -c "transport select swd" \
  -f target/nordic/nrf52.cfg \
  -c "init" -c "halt" -c "nrf5 mass_erase" \
  -c "program xiao-bootloader.hex verify" \
  -c "reset run" -c "exit"
```

**nrfjprog / nRF Util** (J-Link):

```bash
nrfjprog --recover
nrfjprog --program xiao-bootloader.hex --verify --chiperase
nrfjprog --reset
```

**pyOCD** (CMSIS-DAP, ST-Link, picoprobe):

```bash
pyocd erase --chip -t nrf52840
pyocd flash -t nrf52840 xiao-bootloader.hex
```

Expect a mass-erase confirmation, then a verify pass, then reset.

### 6. Confirm

Unplug the XIAO from USB and plug it back in. With flash erased there's no
application to run, so the bootloader stays in DFU mode on its own and the
`XIAO-SENSE` (or `XIAO-BOOT`) drive should mount. Drag any UF2 onto it to
get back to a working firmware.

---

## Avoiding this in the first place

The brick that motivated this document happened on 2026-07-22: `pm_static.yml`
placed `zboss_nvram` at `0xF4000` — exactly where the Adafruit bootloader
lives. The first flash erased it and the board went dark.

Two rules follow, and both are enforced by the current configuration:

- **Never write `0x0`–`0x27000` (MBR + SoftDevice) or `0xF4000`–`0x100000`
  (bootloader).** Any flash-backed storage you add goes inside the app slot.
  See [hardware.md](hardware.md#flash-regions-you-must-never-write).
- **Leave `CONFIG_RESET_ON_FATAL_ERROR` off during first-time integration.**
  With it on, a fault loop resets the board continuously and swallows the
  double-tap window, so a merely-crashing board becomes indistinguishable
  from a bricked one.

---

## Optional: the Raspberry Pi flash rig

Everything below is a bench convenience for people iterating on firmware.
Skip it if you're happy dragging UF2 files onto a drive.

The payoff: after any `west build`, one command from the repo root does the
whole flash cycle.

```bash
./tools/flash.sh                  # standard build output
./tools/flash.sh path/to/other.uf2
```

It SSHes to the Pi, pulses the reset line twice to emulate the bootloader's
double-tap detection, waits for the drive to mount, copies the UF2, and
confirms the drive unmounted again.

There are three flashing scripts for three situations — see
[tools/README](../tools/README.md#flashing) for when to use `flash-serial.sh`
(USB alive but mass-storage silent) and `flash-swd.sh` (USB unplugged
entirely, e.g. during current measurement).

### What you need

- A **Raspberry Pi 3B+ or newer** (not Pi 5 without a config change) on your
  network with SSH access
- Jumper wires, or pogo pins in a jig
- The XIAO powered over USB from your workstation — the Pi supplies SWD
  signals only, never power

### The 3D-printed jig

A holder for spring-loaded pogo pins against the XIAO's five underside pads,
so you don't have to solder or hand-hold probes. Prints in about an hour.

> **The model isn't published yet**, so this section is a parts list rather
> than something you can print today. You don't need it: recovery works fine
> with female Dupont wires held against the pads, or soldered on. The jig
> only saves you a third hand if you're reflashing repeatedly.

BOM:
- 5 × pogo pin contacts, 0.68 mm tip, ~4 mm travel
- 5 × female Dupont jumper wires
- 2 × M2 screws (or press-fit if your tolerances are good)

### Optional bench-input wires

Two extra jumpers let the Pi drive the board's inputs over SSH, so you can
exercise firmware without touching the hardware. Both are active-low with
internal pull-ups; the Pi pulls them low to fire an edge.

| XIAO pin | Pi header pin | Pi BCM | Purpose |
|---|---|---|---|
| D6 (P1.11) | 11 | GPIO 17 | User button — short press joins, long press factory-resets |
| D7 (P1.12) | 13 | GPIO 27 | Bench pulse simulator — bumps the accumulator with no join side effects |

Ground is already shared through the SWD wiring.

### Pi setup

Install OpenOCD (usually present on Bookworm):

```bash
sudo apt-get install openocd
```

Grant passwordless sudo for OpenOCD only — it needs GPIO register access.
Keep the scope narrow; nothing else on the rig should be passwordless.

```bash
echo 'ctadmin ALL=(root) NOPASSWD: /usr/bin/openocd' | sudo tee /etc/sudoers.d/openocd-nopasswd
sudo chmod 440 /etc/sudoers.d/openocd-nopasswd
```

Install the double-tap emulator at `~/xiao-bootloader.sh`:

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

The other Pi-side helpers (`xiao-pulse.sh`, `xiao-short-press.sh`,
`xiao-long-press.sh`) have templates in [`tools/`](../tools/) — copy them to
the Pi's home directory and `chmod +x`.

### Workstation setup

Generate a passphrase-less ed25519 key, add it to the Pi's
`~/.ssh/authorized_keys`, and define the host alias the scripts expect:

```
Host rpi-xiao
    HostName <your-pi-ip>
    User <your-pi-user>
    IdentityFile ~/.ssh/xiao-recovery-rpi
    IdentitiesOnly yes
```

Verify with `ssh rpi-xiao uptime` — it must return without prompting.

---

## Related documents

- **[hardware.md](hardware.md)** — full pinout, sensor and battery wiring
- **[tools/README](../tools/README.md)** — every bench script, including the
  RTT-over-SWD log tail for when USB is unplugged
- **[CONTRIBUTING](../CONTRIBUTING.md)** — toolchain, build, and the sysbuild
  gotcha that masquerades as a brick

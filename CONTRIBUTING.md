# Contributing

Firmware for a Seeed XIAO nRF52840 based Zigbee energy pulse-counter. Built on
Zephyr / nRF Connect SDK with the ncs-zigbee add-on.

Locked design decisions and the rationale for them live in
[`seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md`](seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md).
Read this before proposing architectural changes. In-flight status is under
[`seeed-studio-zigbee-energy-meter/docs/working/`](seeed-studio-zigbee-energy-meter/docs/working/).

---

## Prerequisites

- **OS**: Windows 10/11, macOS (Intel or Apple Silicon), or Linux — all
  supported by Nordic's nRF Connect SDK
- **~5 GB free disk** for the toolchain + SDK
- **Seeed XIAO nRF52840** (plain or Sense) with a USB-C cable for flashing
  and bench testing
- **Zigbee2MQTT coordinator** — only needed for end-to-end network tests
  (not for the walking-skeleton or host tests)

---

## Recommended setup: VS Code + nRF Connect Extension Pack

Cross-platform, Nordic's officially supported flow. Handles toolchain install,
build, flash, and RTT logging without leaving the editor.

### 1. Install VS Code

<https://code.visualstudio.com> — installer for your OS.

### 2. Install the nRF Connect Extension Pack

Extensions panel → search **"nRF Connect for VS Code Extension Pack"**
(publisher: `nordic-semiconductor`). Installing the pack pulls in the toolchain
manager, build UI, serial terminal, RTT viewer, and Kconfig/DTS editors.

### 3. Install a toolchain and SDK

Open the **nRF Connect** side panel (Nordic icon in the activity bar).

- **Manage toolchains** → *Install Toolchain* → pick **NCS v2.9.2**.
- **Manage SDKs** → *Install SDK* → **NCS v2.9.2** (same as the toolchain).

Downloads total ~2–3 GB; expect 10–20 min on a reasonable connection.

**Why v2.9.2 specifically**: this project uses the [ncs-zigbee](https://github.com/nrfconnect/ncs-zigbee) add-on for the R23 Zigbee stack. The current stable add-on release (**v1.3.0**, bundling ZBOSS R23 v4.2.2.3) is pinned to NCS **v2.9.2**. Older ncs-zigbee (v1.0.0–v1.2.0) targets NCS v2.9.0. The nRF54LM20-preview branch targets NCS v3.1.0 but is for a different chip family and does not apply here.

### 4. Add this project as an application

Welcome view → *Applications* → **Add an existing application**.
Point at the `seeed-studio-zigbee-energy-meter/` folder (the one containing
`CMakeLists.txt`).

### 5. Create a build configuration and build

- On the application, click *Add Build Configuration*
- **Board**: `xiao_ble` (or `xiao_ble/nrf52840` on NCS ≥ v2.6 — the picker
  shows what's available)
- Leave the other defaults
- Click *Build Configuration* to build

Success: `build/zephyr/zephyr.uf2` appears.

### 6. Flash the board

The XIAO nRF52840 uses a UF2 bootloader — no debugger needed.

1. Double-tap the small reset button on the XIAO (roughly within 500 ms).
2. Your OS mounts a drive named `XIAO-SENSE` (or similar).
3. Copy `build/zephyr/zephyr.uf2` onto that drive.
4. The drive unmounts, the board reboots into your firmware.

### 7. Watch serial output

After flashing, the board enumerates as a USB CDC-ACM serial port:

- **Windows**: a `COMx` entry (Device Manager → Ports)
- **macOS**: `/dev/tty.usbmodem*`
- **Linux**: `/dev/ttyACM*` (may need `sudo usermod -aG dialout $USER` + relog)

Open it with the *Serial Monitor* extension bundled in the pack (or `screen`,
`minicom`, PuTTY). Baud is ignored by CDC-ACM.

Expected output for the current skeleton:

```
<inf> main: XIAO Zigbee Energy Meter booted
<inf> main: heartbeat=0 accumulator_total=0
<inf> main: heartbeat=1 accumulator_total=0
```

---

## Alternative: CLI-only with `nrfutil`

For contributors who prefer the terminal. Install per Nordic's docs at
<https://www.nordicsemi.com/Products/Development-tools/nRF-Util>.

```bash
nrfutil install sdk-manager toolchain-manager device
nrfutil sdk-manager toolchain install --ncs-version v2.9.2
nrfutil sdk-manager install --ncs-version v2.9.2

# Drop into an env-active shell (west, ZEPHYR_BASE, toolchain on PATH):
nrfutil sdk-manager toolchain launch --ncs-version v2.9.2 --shell

cd seeed-studio-zigbee-energy-meter
west build -b xiao_ble -p always
```

Flash with UF2 as above, or `nrfutil device program --firmware build/zephyr/zephyr.hex` over SWD if you have a debugger attached.

---

## Running the host tests

The pure-logic modules build and run with any C compiler — no Zephyr toolchain
required. Fast local iteration for anything under `src/pulse_accumulator.*` and
future host-testable modules.

```bash
cd seeed-studio-zigbee-energy-meter/tests
make test
```

All host tests must be green before opening a PR that touches host-testable
code.

---

## Project rules

- **TDD for pure logic** — host-testable modules are driven test-first,
  one behaviour per cycle (see the design doc's *Testing* section)
- **Hardware paths** (LPCOMP, PPI, radio, USB, GPIO) — bench-verified;
  unit tests not required
- **Small, single-purpose PRs** — link the design-doc section or open
  working-note the PR implements

## Issues and PRs

- Issues and PRs: <https://github.com/kwood1992/nRF52840-power-meter>
- For bug reports, include: board revision, NCS version, `west build` output
  (or full VS Code build log), and the serial output leading up to the failure.

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
- **Board target**: `xiao_ble/nrf52840/sense` (the XIAO nRF52840 board this
  project targets is the Sense variant — identifiable by the onboard IMU
  and PDM microphone visible on the top side, and by the `XIAO-SENSE`
  volume that mounts in bootloader mode). For a plain XIAO nRF52840 use
  `xiao_ble/nrf52840`.
- **⚠️ Uncheck "Use sysbuild"** — see the Build Gotchas section below.
  Leaving this on will produce firmware that flashes cleanly but silently
  refuses to boot.
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

## Build gotchas

### Sysbuild must be OFF (unless we also enable MCUboot)

NCS 2.9.x's **sysbuild silently ignores `CONFIG_FLASH_LOAD_OFFSET` when
MCUboot is disabled** ([zephyrproject-rtos/zephyr#88802](https://github.com/zephyrproject-rtos/zephyr/issues/88802)).
The app gets linked at VMA `0x0` even though the config says `0x27000`. The
UF2 writes the binary to physical flash `0x27000` (the address the Adafruit
UF2 bootloader expects), but the vector table inside the binary contains
low-range addresses. On boot the bootloader jumps to `0x27000`, reads a
reset-handler pointer like `0x1dd0`, jumps to garbage, silent hard-fault.
No LED, no USB, no serial, board looks dead.

**Symptoms**: firmware flashes cleanly (no I/O error) but the board does
absolutely nothing after. No LED, no USB CDC-ACM device enumerates.

**Fix**:
- In the VS Code build configuration, **uncheck "Use sysbuild"** (or
  equivalent) when creating the config. If the option isn't in the GUI on
  your extension version, add `--no-sysbuild` to Extra west build arguments.
- **Wipe the `build/` folder entirely** — sysbuild leaves cached state that
  survives Pristine Build:
  ```bash
  rm -rf seeed-studio-zigbee-energy-meter/build
  ```
- Rebuild. UF2 output moves from `build/<domain>/zephyr/zephyr.uf2` to
  `build/zephyr/zephyr.uf2`.

If we add MCUboot in future, sysbuild can safely be turned back on.

### Board-qualified targets don't pick up `boards/*.overlay` — use `app.overlay`

When you build for a fully-qualified board target (e.g. `xiao_ble/nrf52840/sense`
rather than plain `xiao_ble`), Zephyr looks for `boards/xiao_ble_nrf52840_sense.overlay`
— not the shorter `boards/xiao_ble.overlay`. Auto-discovery of `app.overlay`
at the project root is also sometimes skipped depending on NCS version and
build config. To avoid guessing, `CMakeLists.txt` in this project **force-loads
`app.overlay`** via `EXTRA_DTC_OVERLAY_FILE` before `find_package(Zephyr)`.
Keep new project-wide DT additions in `app.overlay`; if you need
variant-specific tweaks, add them under `boards/<qualified_name>.overlay`.

**Symptom if this breaks**: the C compiler complains about undeclared
`__device_dts_ord_DT_N_S_zephyr_user_...` or similar generated macros —
that means the DTS overlay wasn't parsed, so the DT-derived macros were
never emitted.

### `usb_enable()` returns `-EALREADY` on NCS 2.9.2

NCS 2.9.2's USB device stack registers a `SYS_INIT` hook that calls
`usb_enable()` before `main()` runs, so calling it again from `main()`
returns `-EALREADY`. Treat that as success; only bail on a real failure.
The current `src/main.c` shows the pattern.

### Verifying a UF2 has a correct vector table

If a rebuild flashes silently, decode the first block of the UF2:

```bash
python3 <<'EOF'
import struct
d = open('seeed-studio-zigbee-energy-meter/build/zephyr/zephyr.uf2', 'rb').read()
target = struct.unpack('<I', d[12:16])[0]
sp     = struct.unpack('<I', d[32:36])[0]
reset  = struct.unpack('<I', d[36:40])[0]
print(f'writes to 0x{target:08x}, initial SP=0x{sp:08x}, reset handler=0x{reset:08x}')
EOF
```

Reset handler must be in the range `0x00027001`–`0x0002FFFF` (Thumb bit set,
inside the app slot). Anything under `0x00027000` means the linker didn't
apply the flash offset — you've hit the sysbuild bug again.

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

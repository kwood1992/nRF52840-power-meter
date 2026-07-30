# Contributing

Firmware for a Seeed XIAO nRF52840 based Zigbee energy pulse-counter. Built on
Zephyr / nRF Connect SDK with the ncs-zigbee add-on.

Locked design decisions and the rationale for them live in
[`seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md`](seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md).
Read this before proposing architectural changes. In-flight status is under
[`seeed-studio-zigbee-energy-meter/docs/working/`](seeed-studio-zigbee-energy-meter/docs/working/).

Wiring and pin assignments are in [`docs/hardware.md`](docs/hardware.md).

**Contents**

- [How to contribute](#how-to-contribute) — branches, commits, PRs, review
- [Prerequisites](#prerequisites) and [toolchain setup](#recommended-setup-vs-code--nrf-connect-extension-pack)
- [Build gotchas](#build-gotchas) — read before debugging a "dead" board
- [Testing](#testing) — the three suites and what each is for

---

## How to contribute

### Before you start

- **Open an issue first** for anything beyond a small fix. It's cheaper to
  disagree about an approach in an issue than in a finished PR.
- **Check the design doc.** Its decisions table is deliberately hard to
  change. Reopening a decision is allowed — it's happened several times —
  but the argument needs to be explicit, and the doc gets updated in the
  same PR as the code.
- **Look for a working note.** `docs/working/` often already contains the
  measurements or dead ends relevant to what you're about to do.

### Branch and commit

`main` is protected: it takes no direct pushes, and merges require a pull
request with green CI. Work on a branch.

Branch names follow the existing history — a type prefix, then a short
slug, optionally with the issue number:

```
feat/62-poll-control-unattended-write
fix/68-69-swd-and-join-test-guards
tooling/verify-what-the-rig-reports
docs/hardware-pinout
```

Commit messages: a short imperative subject line, then a body explaining
*why* rather than *what*. Reference issues with `#NN`. The existing log is
a good guide — it favours explanation over ceremony, and there's no
enforced format beyond that.

### Open the PR

The [PR template](.github/pull_request_template.md) asks for four things.
None is bureaucratic:

1. **What and why**, with the issue linked.
2. **Design-doc reference** — which decision or note this implements, and
   whether it changes one.
3. **Testing** — including *bench* evidence for anything touching hardware.
   A compile is not verification. Paste the RTT/serial output, the Z2M
   behaviour, or the measured current.
4. **Cluster-list impact** — if the Zigbee endpoint's cluster list changed,
   the Z2M converter must change with it, and every paired device needs a
   remove-and-re-pair. This has to be flagged loudly; a silent cluster
   change strands existing installs.

Keep PRs small and single-purpose. Several small PRs that each do one thing
review far better than one that does five.

### What CI runs

Every PR gets:

| Check | What it does | Blocks merge |
|---|---|---|
| `host-tests` | Pure-logic unit tests | Yes |
| `tooling-tests` | Shell tooling regression tests | Yes |
| `shellcheck` | Lints `tools/*.sh` at `warning` severity | Yes |
| `firmware-build` | Full Zephyr build for both board variants | No (advisory) |

The firmware build is advisory because it depends on a large external SDK
fetch that can fail for reasons unrelated to your change. Don't ignore it
when it goes red — check whether it's your change or the network.

It also publishes UF2 and hex artifacts for both board variants, which is
the easiest way to test a PR's firmware without a local toolchain.

### Review

The maintainer reviews. Expect questions about power cost and about what
was verified on hardware — those are the two areas where this project's
bugs have historically hidden. Resolve review conversations before merging;
the ruleset requires it.

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
- **Board target**: `xiao_ble/nrf52840/sense` — the Sense variant,
  identifiable by the onboard IMU and PDM microphone on the top side, and by
  the `XIAO-SENSE` volume that mounts in bootloader mode. The plain
  `xiao_ble/nrf52840` also builds and is covered by CI, but has never been
  run on hardware — use the Sense unless you're deliberately testing the
  plain board.
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

Keep project-wide DT additions in `app.overlay`. For **variant-specific**
tweaks, do *not* reach for `boards/<qualified_name>.overlay` — that is the
auto-discovery this project doesn't trust. Follow the pattern already set by
`overlays/sense.overlay`: put the fragment under `overlays/`, where discovery
will never find it, and append it explicitly from `CMakeLists.txt` guarded on
`BOARD`:

```cmake
if(BOARD MATCHES "sense")
	list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/overlays/sense.overlay")
endif()
```

`BOARD` is a cache variable set from the command line by `west build -b`, so
it is available before `find_package(Zephyr)`.

**Anything that saves power belongs behind a CI assertion.** A skipped
overlay fails silently — the build is clean and the current draw doubles. The
`firmware-build` job checks `devicetree_generated.h` to confirm the Sense IMU
nodes are actually disabled; add a similar assertion for any new fragment
that turns hardware off.

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

Reset handler must land inside the app slot: above `0x00027000` (the MBR +
SoftDevice region) and below `0x000F4000` (where the Adafruit UF2 bootloader
starts). The low bit is the Thumb bit and will be 1.

Anything under `0x00027000` means the linker didn't apply the flash offset —
you've hit the sysbuild bug again.

> An earlier version of this note gave the upper bound as `0x0002FFFF`. That
> was written when the application was a walking skeleton barely 36 KB long;
> the app has since grown past it, so that bound now rejects perfectly good
> firmware. `firmware.yml` runs this same check in CI against the real slot
> bounds.

## Alternative: self-contained west workspace

The repo ships a [`west.yml`](west.yml) manifest that pins NCS v2.9.2 and
ncs-zigbee v1.3.0 (ZBOSS R23 v4.2.2.3). This is the reproducible path —
`west init` produces the exact SDK + Zigbee-stack combination the app is
built against, without a shared NCS install to keep in sync. Use this
for CI, for a fresh machine, or when you want the SDK checkout scoped
next to the project.

Prerequisites: Python 3.10+, `west` (`pip install west`), and a working
Zephyr toolchain (either from `nrfutil sdk-manager toolchain install`
or the VS Code extension pack).

```bash
# 1. Init the workspace pointing at this repo
west init -m https://github.com/kwood1992/nRF52840-power-meter workspace
cd workspace
west update            # ~10–20 min: pulls sdk-nrf + all NCS modules + ncs-zigbee
west zephyr-export

# 2. Build from the app subdirectory
cd nrf52840-power-meter/seeed-studio-zigbee-energy-meter
west build -b xiao_ble/nrf52840/sense --no-sysbuild -p always
```

Flash with UF2 as above, or `west flash` if you have a debugger.

**Sysbuild must stay off** — same reason as the VS Code flow (see the
Build Gotchas section). The `--no-sysbuild` flag is required.

## Alternative: CLI-only with `nrfutil` against a shared NCS install

For contributors with an existing NCS install they want to share across
projects. Install per Nordic's docs at
<https://www.nordicsemi.com/Products/Development-tools/nRF-Util>.

```bash
nrfutil install sdk-manager toolchain-manager device
nrfutil sdk-manager toolchain install --ncs-version v2.9.2
nrfutil sdk-manager install --ncs-version v2.9.2

# Drop into an env-active shell (west, ZEPHYR_BASE, toolchain on PATH):
nrfutil sdk-manager toolchain launch --ncs-version v2.9.2 --shell

cd seeed-studio-zigbee-energy-meter
west build -b xiao_ble/nrf52840/sense --no-sysbuild -p always
```

Flash with UF2 as above, or `nrfutil device program --firmware build/zephyr/zephyr.hex` over SWD if you have a debugger attached.

---

## One-command flash via SWD jig (optional, but great)

For rapid firmware iteration, [`docs/swd-recovery-jig.md`](docs/swd-recovery-jig.md)
documents a Raspberry Pi + 3D-printed spring-pin jig setup that reduces
"double-tap the reset button, wait for drive, drag the UF2" to a single
`./tools/flash.sh` command. The same setup recovers a soft-bricked XIAO
(firmware overwrote the Adafruit UF2 bootloader) via SWD. Skip this
section if you're happy with the manual drag flow.

---

## Testing

Three suites. The first two run in CI and gate merges; the third is you and
a board.

### 1. Host unit tests — pure logic

The pure-logic modules build and run with any C compiler. No Zephyr
toolchain, no hardware, about a second to run.

```bash
make -C seeed-studio-zigbee-energy-meter/tests test
```

Covers the pulse accumulator, persistence policy, button classification and
routing, metering scale, report gating, LED priority, calibration bounds,
and battery-level mapping.

Adding a module? If it can be tested this way, it must be — add it to
`tests/Makefile` alongside the others.

### 2. Tooling tests — the bench scripts

The shell tooling has its own regression suite. `ssh`, `scp` and `sleep` are
stubbed and the remote state is faked, so the real decision logic runs
against canned inputs with no Pi, no hardware, and no network.

```bash
./tools/tests/test-join-logic.sh ./tools/test-join.sh
./tools/tests/test-swd-guards.sh ./tools
./tools/tests/test-por-verify.sh ./tools/xiao-por.sh
```

These exist for one specific class of bug: **tooling that reports success
while the underlying state is wrong.** A failed Z2M poll counted as proof
the device hadn't interviewed yet; a wedged OpenOCD reported as "bus
released"; a power-on-reset that never cleared the debug latch reported as
done. Each produces a confident green result that then silently poisons a
bench measurement — the most expensive failure on this rig, because the
number looks plausible.

Run them if you change anything they cover. They're seeded with real
measured values, so they encode what the rig actually does rather than what
it should do in theory.

Shell changes should also pass the linter CI runs:

```bash
shellcheck -S warning tools/*.sh tools/tests/*.sh
```

### 3. Bench verification — hardware paths

LPCOMP, PPI, the radio, USB, GPIO and NVS cannot be unit-tested. They get
verified on a board, and the PR says what was observed.

Useful entry points:

- `./tools/test-join.sh` — full remove/factory-reset/re-interview cycle with
  a PASS/FAIL result, and `EXPECT_CLUSTERS` to assert the advertised cluster
  list
- `./tools/xiao-pulse.sh` and friends — inject synthetic pulses on D7
- `./tools/measure-power.sh` — current trace annotated with Z2M events
- `./tools/rtt-tail.sh` — firmware log over SWD when USB is unplugged

See [tools/README.md](tools/README.md) for the full set, and
[docs/hardware.md](docs/hardware.md#bench-testing-without-a-meter) for the
D7 injection path.

> Two measurement traps worth knowing before you quote a number: holding D7
> low adds ~0.24 mA through the pin's pull-up (subtract a duty-matched
> control), and an LED torch's PWM flicker causes 3–4× pulse overcounting.

---

## Project rules

- **TDD for pure logic** — host-testable modules are driven test-first, one
  behaviour per cycle (see the design doc's *Testing* section)
- **Hardware paths** (LPCOMP, PPI, radio, USB, GPIO) — bench-verified; unit
  tests not required, bench evidence in the PR is
- **Power cost is a review criterion** — the device targets multi-year AAA
  life. Anything that adds a wake, holds a peripheral enabled, or keeps
  HFCLK running needs justifying. Several past bugs were a peripheral left
  enabled costing ~1.5 mA.
- **Small, single-purpose PRs** — link the design-doc section or working
  note the PR implements
- **The converter moves with the firmware** — the Z2M external converter in
  `external-converters/` is half the device interface. A cluster or
  attribute change that lands without it produces a device that pairs and
  then exposes nothing useful.

## Reporting bugs

Use the [issue templates](https://github.com/kwood1992/nRF52840-power-meter/issues/new/choose).
For firmware bugs, include the board variant, NCS version, how the board is
powered, the build output, and the serial or RTT log leading up to the
failure.

Two things that look like bugs but usually aren't:

- **Board flashes cleanly then does nothing** — almost always the sysbuild
  linker bug above, not a brick.
- **Z2M shows a stale cluster list or won't re-interview** — Z2M serves that
  data from its own database. Remove and re-pair; a device-side factory
  reset doesn't invalidate it.

# Self-contained west workspace (issue #3)

## What was added

- `west.yml` at repo root. T2 topology: this repo is the manifest repo,
  sdk-nrf is imported wholesale, ncs-zigbee is added as an extra
  project at `modules/lib/ncs-zigbee`.
- `CONTRIBUTING.md` — new section "Alternative: self-contained west
  workspace" with the `west init` flow. The VS Code + nRF Connect
  extension flow remains the default; the existing nrfutil flow is
  retained and reframed as "against a shared NCS install."

## Version pinning

| Component | Version | Why |
|---|---|---|
| `sdk-nrf` | v2.9.2 | Only NCS release that ncs-zigbee v1.3.0 targets. |
| `ncs-zigbee` | v1.3.0 | Current stable; bundles ZBOSS R23 v4.2.2.3. |

These two must move together — ncs-zigbee v1.2.x targets NCS v2.9.0,
and the v3.x preview branch targets NCS v3.1.0 on nRF54LM20 (different
chip family). Do not upgrade either in isolation.

## What was NOT verified

- **`west init -m … && west update && west build` was not run end-to-end.**
  This sandbox doesn't have a west install + toolchain provisioned, so
  I couldn't run the full flow. The manifest follows the standard
  NCS T2 pattern (identical shape to Nordic's own sample projects that
  use ncs-zigbee), but a first contributor running this cold will be
  the first real proof.
- The `--no-sysbuild` build flag is required (documented gotcha —
  sysbuild silently ignores `FLASH_LOAD_OFFSET` on xiao_ble when
  MCUboot is disabled). CONTRIBUTING.md keeps that warning next to
  the new flow.

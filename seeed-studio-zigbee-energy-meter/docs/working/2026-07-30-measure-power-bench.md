# `measure-power.sh` bench verification (#35)

End-to-end check of the workflow-polish half of #35 (the underlying
INA219 sampler + POR relay already shipped separately): orchestrator +
Z2M event tagger + Mac-side matplotlib plotter. Runs against the
already-standing rpi-xiao rig (BCM 22 relay in the 3V3 → BAT path;
SWD attached this session, so the ~1.5 mA CoreSight anchor is present
in the traces).

## Test 1 — isolated `z2m-events.py`

Streams `mosquitto_sub -v` against both `bridge/event` and the device's
topic. Verified the deadline is honored (empty 8 s run exits at
~8.6 s including SSH overhead) and that both topic types parse:

- **Device state publish** (real sleepy-ED report while the rig was
  quiet):
  ```
  1785371268872,zigbee2mqtt/0xf4ce361b0656e80e,energy,3504.04
  1785371268872,zigbee2mqtt/0xf4ce361b0656e80e,imp_per_kwh,800
  1785371268872,zigbee2mqtt/0xf4ce361b0656e80e,linkquality,51
  1785371268872,zigbee2mqtt/0xf4ce361b0656e80e,min_pulse_width_us,2500
  1785371268872,zigbee2mqtt/0xf4ce361b0656e80e,power,0
  ```
  Each state key gets its own row so the plotter can annotate any of
  them independently.
- **Bridge event** (manually injected via `z2m-cli pub bridge/event
  '{"type":"device_joined","data":{...}}'`):
  ```
  1785371492599,zigbee2mqtt/bridge/event,device_joined,{"ieee_address": "0xf4ce361b0656e80e"; "friendly_name": "test"}
  ```
  Commas inside the JSON `data` blob are replaced with semicolons in
  `emit()` so the CSV stays four-column parseable without quoting.

**Fixes landed while bench-testing:**

- `z2m-events.py` originally invoked `~/z2m-cli sub -v bridge/event
  <ieee>` — but z2m-cli's arg order is `sub <suffix> [extras]`, so `-v`
  ended up as the suffix and the subscription was silently broken. Also,
  z2m-cli only takes one `-t` so bridge/event + device topic can't share
  one call. Rewritten to source `~/.mosquitto-xiao-creds` and invoke
  `mosquitto_sub` directly with both `-t` flags.
- The `for line in proc.stdout` loop blocked on I/O; `--duration` was
  dead code whenever there was no MQTT traffic. Replaced with
  `select.select([fd], [], [], 0.5)` so the deadline check runs every
  500 ms regardless of subscription traffic.

## Test 2 — isolated `plot-power.py`

Plotted the existing 180 s sleepy-ED baseline CSV
(`ina219-2026-07-29-185443-sleepy-ed-baseline.csv`) — output matched
the profile documented in `2026-07-29-sleepy-ed-baseline.md` (mostly
below the noise floor with one 3.4 mA poll-catch spike).

**Fix landed:** the `date +%z` format on the Pi emits `+HHMM` (no
colon), which Python 3.9's `datetime.fromisoformat` rejects. Added a
regex normalization `+HHMM → +HH:MM` before parsing.

## Test 3 — full `measure-power.sh` orchestrator

```
$ tools/measure-power.sh with-events 30 10
measuring: label=with-events duration=30s rate=10Hz ieee=0xf4ce361b0656e80e
output:    …/docs/working/measurements/2026-07-30-103422-with-events/
# summary n=301 mean=1.731mA sd=0.164mA min=1.100mA p50=1.700mA p95=2.000mA max=2.100mA
wrote …/plot.png
done
```

Injected two `bridge/event` publishes (`device_joined` at t≈8 s,
`device_interview_successful` at t≈16 s) during the window. Both
landed as red vertical lines with rotated labels at the exact
wall-clock positions in the rendered PNG (`plot.png` in the
`with-events` measurement dir).

Artifact layout as designed:
```
docs/working/measurements/2026-07-30-103422-with-events/
├── events.csv
├── ina219.csv
└── plot.png
```

## AC status for #35

- [x] INA219 wired on the shunt, `i2cdetect` shows `0x40` — pre-existing rig
- [x] Sampler (`ina219-sample.sh` / `.py`) working and shipping CSVs to `docs/working/` — pre-existing
- [x] Z2M event tagger — this session (`tools/z2m-events.py` + orchestrator)
- [x] Mac-side wrapper (`measure-power.sh` with `start/stop/plot`
      semantics — implemented as `<label> <seconds> <hz>` one-shot,
      matching the actual bench usage pattern where users don't run
      long-lived systemd services but ad-hoc timestamped captures)
- [x] Baseline capture — pre-existing baseline in
      `2026-07-29-sleepy-ed-baseline.md` (mean ≈ 0 mA post-POR /
      SWD-detached); this session's captures include SWD's ~1.5 mA
      overhead (documented, expected, and matches
      `project_battery_current_hfclk_anchor`)
- [x] Documented ~10 µA floor limitation — inherited from
      `ina219-sample.py` config and referenced in the sleepy-ED baseline
      writeup; PPK2 is the escalation for sub-50 µA work

Closable.

## Related

- Bench artifact: `docs/working/measurements/2026-07-30-103422-with-events/plot.png`
- Ticket: #35
- Underlying rig memory: [[reference_ina219_pi_harness]],
  [[project_battery_current_hfclk_anchor]]

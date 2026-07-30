#!/usr/bin/env python3
"""Stream Z2M events tagged with wall-clock timestamps as CSV on stdout.

Runs on the Pi (rpi-xiao). Uses ~/z2m-cli (already carries broker + creds)
to subscribe to `bridge/event` and the XIAO's device topic; each MQTT
message becomes one CSV row with a POSIX-ms wallclock so plot-power.py
can align it to the INA219 sample stream on a shared wall-clock axis.

Usage:
  z2m-events.py --ieee 0xf4ce361b0656e80e [--duration 60]

Row format: wallclock_ms,topic_slug,event_or_field,value
  - bridge/event rows: event_or_field=<event.type>, value=<data JSON blob>
  - device rows: event_or_field=<state key>, value=<state val> (one row per key
    in the state publish, so `energy` / `power` / `min_pulse_width_us` / …
    each land as their own row and can be annotated independently)
"""
import argparse
import json
import os
import select
import signal
import subprocess
import sys
import time
from typing import Optional


def emit(wallclock_ms: int, topic: str, field: str, value) -> None:
    val_str = str(value).replace(",", ";")  # keep CSV parseable without quoting
    print(f"{wallclock_ms},{topic},{field},{val_str}")
    sys.stdout.flush()


def parse_message(line: str) -> Optional[tuple]:
    """z2m-cli `sub -v` prints '<topic> <payload>' per message."""
    line = line.rstrip("\n")
    if not line:
        return None
    parts = line.split(" ", 1)
    if len(parts) < 2:
        return None
    return parts[0], parts[1]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ieee", required=True, help="XIAO IEEE address (e.g. 0xf4ce361b0656e80e)")
    ap.add_argument("--duration", type=float, default=0.0,
                    help="seconds to run before exit (0 = until SIGTERM)")
    ap.add_argument("--note", default="", help="free-form context for the CSV header")
    args = ap.parse_args()

    started = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    started_wallclock_ms = int(time.time() * 1000)
    print(f"# z2m-events started={started} started_wallclock_ms={started_wallclock_ms}", file=sys.stdout)
    print(f"# ieee={args.ieee} duration_s={args.duration}", file=sys.stdout)
    if args.note:
        print(f"# note={args.note}", file=sys.stdout)
    print("wallclock_ms,topic,field,value", file=sys.stdout)
    sys.stdout.flush()

    # Subscribe to bridge/event + the device topic in one mosquitto_sub
    # invocation. z2m-cli only takes one -t so we bypass it and read the
    # same creds file it does. -v gives `topic message` per line so we
    # can attribute each row to its source topic.
    cmd = (
        "set -eu; . ~/.mosquitto-xiao-creds; "
        f"exec mosquitto_sub -h \"$BROKER_HOST\" -p \"$BROKER_PORT\" "
        f"-u \"$BROKER_USER\" -P \"$BROKER_PASS\" -v "
        f"-t \"$Z2M_BASE_TOPIC/bridge/event\" "
        f"-t \"$Z2M_BASE_TOPIC/{args.ieee}\""
    )
    proc = subprocess.Popen(
        ["bash", "-lc", cmd],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )

    deadline = time.monotonic() + args.duration if args.duration > 0 else None

    def stop_and_exit(_signum=None, _frame=None) -> None:
        try:
            proc.send_signal(signal.SIGTERM)
            proc.wait(timeout=2)
        except Exception:
            proc.kill()
        sys.exit(0)

    signal.signal(signal.SIGTERM, stop_and_exit)
    signal.signal(signal.SIGINT, stop_and_exit)

    assert proc.stdout is not None
    fd = proc.stdout.fileno()
    try:
        while True:
            if deadline is not None and time.monotonic() > deadline:
                break
            # Non-blocking read with 0.5s poll so --duration is honored even
            # when there's no MQTT traffic on either subscribed topic.
            ready, _, _ = select.select([fd], [], [], 0.5)
            if not ready:
                continue
            line = proc.stdout.readline()
            if not line:
                break
            parsed = parse_message(line)
            if parsed is None:
                continue
            topic, payload = parsed
            wallclock_ms = int(time.time() * 1000)
            # Split payload: bridge/event → {type, data}; device state → flat dict.
            try:
                doc = json.loads(payload)
            except json.JSONDecodeError:
                emit(wallclock_ms, topic, "_raw", payload)
                continue
            if topic.endswith("/bridge/event") or topic == "bridge/event":
                ev_type = doc.get("type", "unknown")
                emit(wallclock_ms, topic, ev_type, json.dumps(doc.get("data", {})))
            elif isinstance(doc, dict):
                for k, v in doc.items():
                    emit(wallclock_ms, topic, k, v)
            else:
                emit(wallclock_ms, topic, "_scalar", doc)
    finally:
        stop_and_exit()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Plot an INA219 current-over-time CSV with Z2M events annotated as vlines.

Runs on the Mac. Reads the sampler CSV produced by ina219-sample.py and
optionally the tagger CSV from z2m-events.py; if the events file is
provided, the wall-clock start recorded in the INA219 CSV header
(`# ina219-sample started=<ISO>`) is used to convert the sampler's
monotonic timestamps to wall-clock time, matching the tagger's rows.

Usage:
  plot-power.py <ina219.csv> [--events <events.csv>] [--out <png>]
"""
import argparse
import csv
import datetime as dt
import os
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import matplotlib.dates as mdates
except ImportError:
    sys.stderr.write("matplotlib not found. Install: pip3 install matplotlib\n")
    sys.exit(2)


def parse_ina219(path: Path):
    start_iso = None
    rows = []
    with path.open() as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                for token in line.split():
                    if token.startswith("started="):
                        start_iso = token[len("started="):]
                continue
            if not line or line.startswith("timestamp_ms"):
                continue
            parts = line.split(",")
            if len(parts) < 4:
                continue
            ts_ms = int(parts[0])
            current_mA = float(parts[3])
            rows.append((ts_ms, current_mA))
    if start_iso is None:
        raise ValueError(f"{path}: no `# ... started=<ISO>` header — can't align to wallclock")
    start_dt = dt.datetime.fromisoformat(start_iso)
    return start_dt, rows


def parse_events(path: Path):
    rows = []
    with path.open() as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#") or not line or line.startswith("wallclock_ms"):
                continue
            parts = line.split(",", 3)
            if len(parts) < 4:
                continue
            wallclock_ms = int(parts[0])
            topic, field, value = parts[1], parts[2], parts[3]
            rows.append((wallclock_ms, topic, field, value))
    return rows


def annotate_key(topic: str, field: str) -> str:
    """Which events warrant a vline. Empty string = skip."""
    if field in {"device_joined", "device_interview_started", "device_interview_successful",
                 "device_interview_failed", "device_leave", "device_announce"}:
        return field
    if field in {"energy", "min_pulse_width_us", "imp_per_kwh", "power"}:
        return f"{field}"
    return ""


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ina219_csv", type=Path)
    ap.add_argument("--events", type=Path, help="companion z2m-events CSV")
    ap.add_argument("--out", type=Path, help="output PNG (default: same dir as ina219 CSV, .png suffix)")
    args = ap.parse_args()

    if not args.ina219_csv.exists():
        sys.stderr.write(f"not found: {args.ina219_csv}\n")
        return 2

    start_dt, samples = parse_ina219(args.ina219_csv)
    if not samples:
        sys.stderr.write("no samples parsed\n")
        return 3

    xs = [start_dt + dt.timedelta(milliseconds=ts) for ts, _ in samples]
    ys = [c for _, c in samples]

    fig, ax = plt.subplots(figsize=(12, 5))
    ax.plot(xs, ys, linewidth=0.8, color="tab:blue")
    ax.set_xlabel("wall-clock")
    ax.set_ylabel("current (mA)")
    ax.set_title(f"{args.ina219_csv.name} — {len(samples)} samples, span {(xs[-1]-xs[0]).total_seconds():.1f}s")
    ax.grid(True, alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M:%S"))

    if args.events and args.events.exists():
        events = parse_events(args.events)
        for wallclock_ms, topic, field, value in events:
            label = annotate_key(topic, field)
            if not label:
                continue
            event_dt = dt.datetime.fromtimestamp(wallclock_ms / 1000.0, tz=start_dt.tzinfo)
            if event_dt < xs[0] or event_dt > xs[-1]:
                continue
            ax.axvline(event_dt, color="tab:red", alpha=0.4, linewidth=0.8)
            ax.text(event_dt, ax.get_ylim()[1] * 0.95, f"{label}",
                    rotation=90, fontsize=7, verticalalignment="top", color="tab:red")

    out = args.out or args.ina219_csv.with_suffix(".png")
    fig.tight_layout()
    fig.savefig(out, dpi=120)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

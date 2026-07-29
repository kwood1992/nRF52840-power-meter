#!/usr/bin/env python3
"""Stream INA219 samples as CSV on stdout. Runs on the Pi (rpi-xiao)."""
import argparse
import sys
import time

from smbus2 import SMBus, i2c_msg


REG_CONFIG = 0x00
REG_SHUNT = 0x01
REG_BUS = 0x02
POR_CONFIG = 0x399F


def read_reg(bus: SMBus, addr: int, reg: int) -> int:
    w = i2c_msg.write(addr, [reg])
    r = i2c_msg.read(addr, 2)
    bus.i2c_rdwr(w, r)
    b = bytes(r)
    return (b[0] << 8) | b[1]


def sign16(v: int) -> int:
    return v - (1 << 16) if v & 0x8000 else v


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hz", type=float, default=10.0, help="samples per second")
    ap.add_argument("--duration", type=float, default=60.0, help="seconds")
    ap.add_argument("--bus", type=int, default=1)
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0x40)
    ap.add_argument("--shunt-ohms", type=float, default=0.1)
    ap.add_argument("--note", default="", help="free-form context for the CSV header")
    args = ap.parse_args()

    period = 1.0 / args.hz
    n_expected = int(args.hz * args.duration)

    with SMBus(args.bus) as bus:
        cfg = read_reg(bus, args.addr, REG_CONFIG)
        # Header (comment lines the analysis tools skip).
        started = time.strftime("%Y-%m-%dT%H:%M:%S%z")
        print(f"# ina219-sample started={started}", file=sys.stdout)
        print(f"# bus={args.bus} addr=0x{args.addr:02X} shunt_ohms={args.shunt_ohms}", file=sys.stdout)
        print(f"# config=0x{cfg:04X} por_default=0x{POR_CONFIG:04X}", file=sys.stdout)
        print(f"# hz={args.hz} duration_s={args.duration} n_expected={n_expected}", file=sys.stdout)
        if args.note:
            print(f"# note={args.note}", file=sys.stdout)
        print("timestamp_ms,bus_mV,shunt_uV,current_mA,ready,overflow", file=sys.stdout)
        sys.stdout.flush()

        currents_mA: list[float] = []
        t0 = time.monotonic()
        deadline = t0 + args.duration
        next_tick = t0
        while True:
            now = time.monotonic()
            if now >= deadline:
                break
            if now < next_tick:
                time.sleep(next_tick - now)
            next_tick += period

            sample_t = time.monotonic()
            shunt_raw = sign16(read_reg(bus, args.addr, REG_SHUNT))
            bus_raw = read_reg(bus, args.addr, REG_BUS)
            shunt_uV = shunt_raw * 10  # LSB = 10 uV
            bus_mV = (bus_raw >> 3) * 4  # LSB = 4 mV
            ready = (bus_raw >> 1) & 1
            overflow = bus_raw & 1
            current_mA = (shunt_uV / 1000.0) / args.shunt_ohms  # mV / ohms = mA
            currents_mA.append(current_mA)

            ts_ms = int((sample_t - t0) * 1000)
            print(f"{ts_ms},{bus_mV},{shunt_uV},{current_mA:.3f},{ready},{overflow}")
            sys.stdout.flush()

    # Summary to stderr so it doesn't pollute the CSV.
    if currents_mA:
        n = len(currents_mA)
        mean = sum(currents_mA) / n
        cmin = min(currents_mA)
        cmax = max(currents_mA)
        srt = sorted(currents_mA)
        p50 = srt[n // 2]
        p95 = srt[min(n - 1, int(n * 0.95))]
        var = sum((c - mean) ** 2 for c in currents_mA) / n
        sd = var ** 0.5
        print(
            f"# summary n={n} mean={mean:.3f}mA sd={sd:.3f}mA "
            f"min={cmin:.3f}mA p50={p50:.3f}mA p95={p95:.3f}mA max={cmax:.3f}mA",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Fire N precisely-timed pulses on the Pi's XIAO D7 bench-inject wire.

Runs on the Pi (rpi-xiao). Uses pigpio's wave-DMA to hit sub-microsecond
edge accuracy so the min-pulse-width filter (#59) can be exercised at
threshold ± 100 µs, which shell-driven `pinctrl` (5–15 ms per fork/exec)
can't reach.

Pulse shape: idle HIGH → LOW for --pulse-us → HIGH for --gap-us → repeat.
The XIAO's D7 GPIOTE sees a HITOLO edge on entry and a LOTOHI edge on
exit; the impl-1 TIMER3 chain gates the LOTOHI-triggered TIMER2 COUNT on
whether the interval exceeds CONFIG_APP_PULSE_MIN_WIDTH_US (or the
NVS/Zigbee override).

Prerequisites:
  sudo apt install python3-pigpio
  sudo systemctl enable --now pigpiod
"""
import argparse
import sys
import time

try:
    import pigpio
except ImportError:
    sys.stderr.write(
        "pigpio module not found. On Raspberry Pi OS:\n"
        "  sudo apt install python3-pigpio\n"
        "  sudo systemctl enable --now pigpiod\n"
    )
    sys.exit(2)

PIN_DEFAULT = 27


def build_pulse(pin: int, pulse_us: int, gap_us: int) -> list:
    return [
        pigpio.pulse(0, 1 << pin, pulse_us),
        pigpio.pulse(1 << pin, 0, gap_us),
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--count", type=int, required=True, help="number of pulses")
    ap.add_argument("--pulse-us", type=int, required=True, help="LOW pulse width in microseconds")
    ap.add_argument("--gap-us", type=int, default=5000,
                    help="HIGH gap between pulses in microseconds (default 5000)")
    ap.add_argument("--pin", type=int, default=PIN_DEFAULT, help=f"BCM GPIO (default {PIN_DEFAULT})")
    args = ap.parse_args()

    if args.count < 1 or args.pulse_us < 1 or args.gap_us < 1:
        sys.stderr.write("count/pulse-us/gap-us must all be >= 1\n")
        return 2
    if args.count > 65535:
        # pigpio wave_chain loop counter is a 16-bit little-endian pair;
        # split into multiple invocations if a bigger run is ever needed.
        sys.stderr.write("count > 65535 not supported by a single wave_chain loop; split the run\n")
        return 2

    pi = pigpio.pi()
    if not pi.connected:
        sys.stderr.write(
            "pigpio daemon not reachable. Start it with: sudo systemctl start pigpiod\n"
        )
        return 3

    try:
        pi.set_mode(args.pin, pigpio.OUTPUT)
        pi.write(args.pin, 1)

        pi.wave_clear()
        pi.wave_add_generic(build_pulse(args.pin, args.pulse_us, args.gap_us))
        wid = pi.wave_create()
        if wid < 0:
            sys.stderr.write(f"wave_create failed: {wid}\n")
            return 4

        if args.count == 1:
            chain = [wid]
        else:
            lo = args.count & 0xFF
            hi = (args.count >> 8) & 0xFF
            chain = [255, 0, wid, 255, 1, lo, hi]

        pi.wave_chain(chain)

        expected_s = args.count * (args.pulse_us + args.gap_us) / 1_000_000.0
        deadline = time.monotonic() + expected_s + 1.0
        while pi.wave_tx_busy():
            if time.monotonic() > deadline:
                sys.stderr.write("wave_tx_busy timeout — bailing\n")
                return 5
            time.sleep(0.001)

        pi.set_mode(args.pin, pigpio.INPUT)
        pi.set_pull_up_down(args.pin, pigpio.PUD_UP)
        pi.wave_delete(wid)

        print(
            f"sent {args.count} pulse(s): pulse_us={args.pulse_us} "
            f"gap_us={args.gap_us} on BCM {args.pin}"
        )
    finally:
        pi.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())

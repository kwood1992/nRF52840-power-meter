#!/usr/bin/env python3
"""Fire N precisely-timed pulses on the Pi's XIAO D7 bench-inject wire.

Runs on the Pi (rpi-xiao). Writes GPSET/GPCLR registers directly through
/dev/gpiomem and busy-waits on time.perf_counter_ns() between edges. No
pigpiod / RPi.GPIO / lgpio dependency — just Python's stdlib and the
`gpio` group membership /dev/gpiomem is granted by default on Raspberry
Pi OS.

Precision is limited by Python-thread preemption + GIL wake latency
(~10–50 µs jitter on an otherwise-idle Pi 3B+), which is well inside the
±100 µs discrimination the #59 AC bench test needs (compare 900 µs vs
1100 µs pulses against a 1000 µs threshold).

Assumes the BCM2837/BCM2711 GPIO peripheral layout (Pi 3 / Pi 4). Pi 5's
RP1 chip uses a different controller and this tool would need a rewrite.

Pulse shape: idle HIGH → LOW for --pulse-us → HIGH for --gap-us → repeat.
The XIAO's D7 GPIOTE sees a HITOLO edge on entry and a LOTOHI edge on
exit; the impl-1 TIMER3 chain gates the LOTOHI-triggered TIMER2 COUNT on
whether the interval exceeds CONFIG_APP_PULSE_MIN_WIDTH_US (or the
NVS/Zigbee override).

Usage:
  xiao-pulse-us.py --count 1000 --pulse-us 900 --gap-us 5000
"""
import argparse
import mmap
import os
import struct
import subprocess
import sys
import time

# BCM2837 / BCM2711 GPIO register offsets within the /dev/gpiomem window.
GPSET0 = 0x1C
GPCLR0 = 0x28
GPIO_MEM_SIZE = 4096

PIN_DEFAULT = 27


def setup_output_high(pin: int) -> None:
    """Configure `pin` as OUTPUT driven HIGH via pinctrl.

    Reuses pinctrl for setup so the idle state matches xiao-pulse.sh and
    we don't have to re-implement the GPFSEL read-modify-write in Python
    where timing precision doesn't matter.
    """
    subprocess.run(["pinctrl", "set", str(pin), "op", "dh"], check=True)


def restore_input_pullup(pin: int) -> None:
    """Return `pin` to input-with-pullup so we're not sourcing current
    into the XIAO's D7. Matches xiao-pulse.sh's exit state."""
    subprocess.run(["pinctrl", "set", str(pin), "ip", "pu"], check=True)


def busy_wait_ns(target_ns: int) -> None:
    while time.perf_counter_ns() < target_ns:
        pass


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
    if args.pin < 0 or args.pin > 31:
        sys.stderr.write("pin must be in 0..31 (this tool writes GPSET0/GPCLR0 only)\n")
        return 2

    setup_output_high(args.pin)

    pin_mask = 1 << args.pin
    set_word = struct.pack("<I", pin_mask)

    try:
        fd = os.open("/dev/gpiomem", os.O_RDWR | os.O_SYNC)
    except PermissionError:
        sys.stderr.write("permission denied on /dev/gpiomem — user must be in the 'gpio' group\n")
        return 3

    try:
        mem = mmap.mmap(fd, GPIO_MEM_SIZE, flags=mmap.MAP_SHARED,
                        prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=0)
    finally:
        os.close(fd)

    try:
        pulse_ns = args.pulse_us * 1000
        gap_ns = args.gap_us * 1000

        t = time.perf_counter_ns()
        for _ in range(args.count):
            mem[GPCLR0:GPCLR0 + 4] = set_word
            t += pulse_ns
            busy_wait_ns(t)
            mem[GPSET0:GPSET0 + 4] = set_word
            t += gap_ns
            busy_wait_ns(t)

        print(
            f"sent {args.count} pulse(s): pulse_us={args.pulse_us} "
            f"gap_us={args.gap_us} on BCM {args.pin}"
        )
    finally:
        mem.close()
        restore_input_pullup(args.pin)
    return 0


if __name__ == "__main__":
    sys.exit(main())

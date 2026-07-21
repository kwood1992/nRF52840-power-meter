# nRF52840 Power Meter

Firmware for a battery-powered Zigbee sensor that watches the pulse LED on a
household electricity meter, counts the flashes, and reports cumulative energy
(kWh) to Home Assistant via Zigbee2MQTT every 5 minutes.

Built on the **Seeed XIAO nRF52840** with the nRF Connect SDK (Zephyr) and the
ncs-zigbee add-on. The pulse pipeline runs entirely in hardware peripherals
(LPCOMP → PPI → TIMER) during System-ON sleep, targeting multi-year AAA
battery life.

## Status

Early development. Pure-logic core (pulse accumulator with 32-bit HW-wrap and
48-bit overflow handling) is done and host-tested. Zephyr walking-skeleton
compiles and boots to a USB CDC-ACM heartbeat log. Zigbee stack, pulse-source
wiring, and low-power paths are next.

See [`seeed-studio-zigbee-energy-meter/docs/working/`](seeed-studio-zigbee-energy-meter/docs/working/)
for current in-flight status.

## Documentation

- **[Design doc](seeed-studio-zigbee-energy-meter/docs/seeed-studio-zigbee-energy-meter.md)** — locked decisions, BOM, firmware structure, testing strategy, and open risks. Read this first.
- **[Contributing / dev environment setup](CONTRIBUTING.md)** — cross-platform toolchain install, build, flash, and host-test workflow.
- **[Working notes](seeed-studio-zigbee-energy-meter/docs/working/)** — living status snapshots.

## License

[GNU GPL](LICENSE).

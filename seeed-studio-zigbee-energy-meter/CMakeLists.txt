cmake_minimum_required(VERSION 3.20.0)

# Force-load our project overlay regardless of board target. Zephyr's
# auto-discovery of app.overlay doesn't fire reliably across every NCS
# version / board-target combo (in particular, board-qualified targets like
# xiao_ble/nrf52840/sense skip it), so we set it explicitly.
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/app.overlay")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_energy_meter LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/pulse_accumulator.c
)

target_include_directories(app PRIVATE src)

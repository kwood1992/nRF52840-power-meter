cmake_minimum_required(VERSION 3.20.0)

# Force-load our project overlay regardless of board target. Zephyr's
# auto-discovery of app.overlay doesn't fire reliably across every NCS
# version / board-target combo (in particular, board-qualified targets like
# xiao_ble/nrf52840/sense skip it), so we set it explicitly.
list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/app.overlay")

# Sense-only devicetree fragment. app.overlay is loaded for every target,
# so anything referencing nodes that exist only on the Sense variant (the
# LSM6DS3TR-C IMU and its regulator) has to be split out — otherwise the
# plain xiao_ble/nrf52840 build dies at DTS parse time on an undefined
# node label. See #75.
#
# Appended explicitly rather than named boards/xiao_ble_nrf52840_sense.overlay
# and left to auto-discovery, for the same reason app.overlay is force-loaded
# above: discovery has not been reliable here across NCS versions and
# board-qualified targets, and silently skipping this one would leave the IMU
# regulator drawing ~0.6-1.7 mA with a clean build log. The firmware-build CI
# job asserts against the generated devicetree that these nodes are actually
# disabled on a Sense build.
#
# BOARD is a cache variable set from the command line (-DBOARD=..., which is
# what `west build -b` passes), so it is already available here, before
# find_package(Zephyr).
if(BOARD MATCHES "sense")
	list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/overlays/sense.overlay")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zigbee_energy_meter LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/battery_level.c
	src/button_action.c
	src/button_press_classifier.c
	src/calibration.c
	src/hw_pulse_counter.c
	src/led_controller.c
	src/led_priority.c
	src/metering_scale.c
	src/nvs_store.c
	src/persist_policy.c
	src/pulse_accumulator.c
	src/report_gate.c
	src/zigbee_app.c
)

target_include_directories(app PRIVATE src)

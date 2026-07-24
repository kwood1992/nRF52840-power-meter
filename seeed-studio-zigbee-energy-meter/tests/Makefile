CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -Werror -O0 -g -I../src
BUILD   := build

TESTS := test_pulse_accumulator test_pulse_edge_detector test_persist_policy test_button_press_classifier test_metering_scale test_report_gate

.PHONY: all test clean
all: test

test: $(addprefix $(BUILD)/,$(TESTS))
	@set -e; for t in $^; do echo "==> $$t"; ./$$t; done
	@echo "OK"

$(BUILD)/test_pulse_accumulator: test_pulse_accumulator.c ../src/pulse_accumulator.c ../src/pulse_accumulator.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pulse_accumulator.c ../src/pulse_accumulator.c

$(BUILD)/test_pulse_edge_detector: test_pulse_edge_detector.c ../src/pulse_edge_detector.c ../src/pulse_edge_detector.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_pulse_edge_detector.c ../src/pulse_edge_detector.c

$(BUILD)/test_persist_policy: test_persist_policy.c ../src/persist_policy.c ../src/persist_policy.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_persist_policy.c ../src/persist_policy.c

$(BUILD)/test_button_press_classifier: test_button_press_classifier.c ../src/button_press_classifier.c ../src/button_press_classifier.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_button_press_classifier.c ../src/button_press_classifier.c

$(BUILD)/test_metering_scale: test_metering_scale.c ../src/metering_scale.c ../src/metering_scale.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_metering_scale.c ../src/metering_scale.c

$(BUILD)/test_report_gate: test_report_gate.c ../src/report_gate.c ../src/report_gate.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_report_gate.c ../src/report_gate.c

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

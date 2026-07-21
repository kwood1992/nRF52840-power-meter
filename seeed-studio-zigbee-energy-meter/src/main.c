#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_device.h>

#include "pulse_accumulator.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void wait_for_host_dtr(const struct device *cdc)
{
	uint32_t dtr = 0;

	while (!dtr) {
		uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
		k_sleep(K_MSEC(100));
	}
}

int main(void)
{
	const struct device *const cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

	if (!device_is_ready(cdc)) {
		return -1;
	}

	if (usb_enable(NULL)) {
		return -1;
	}

	wait_for_host_dtr(cdc);

	LOG_INF("XIAO Zigbee Energy Meter booted");

	struct pulse_accumulator acc;

	pulse_accumulator_init(&acc);

	uint32_t heartbeat = 0;

	while (1) {
		k_sleep(K_SECONDS(1));
		LOG_INF("heartbeat=%u accumulator_total=%llu",
			heartbeat++,
			(unsigned long long)pulse_accumulator_total(&acc));
	}

	return 0;
}

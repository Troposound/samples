/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(blinky, LOG_LEVEL_INF);

#define SLEEP_MS 500

static const struct gpio_dt_spec leds[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
};

static const char *const names[] = { "red", "green", "blue" };

int main(void)
{
	size_t i = 0;

	LOG_INF("Blinky on %s", CONFIG_BOARD_TARGET);

	for (size_t n = 0; n < ARRAY_SIZE(leds); n++) {
		if (!gpio_is_ready_dt(&leds[n])) {
			LOG_ERR("LED %s not ready", names[n]);
			return 0;
		}

		if (gpio_pin_configure_dt(&leds[n], GPIO_OUTPUT_INACTIVE) < 0) {
			LOG_ERR("LED %s configure failed", names[n]);
			return 0;
		}
	}

	while (1) {
		gpio_pin_set_dt(&leds[i], 1);
		k_msleep(SLEEP_MS);
		gpio_pin_set_dt(&leds[i], 0);
		i = (i + 1) % ARRAY_SIZE(leds);
	}

	return 0;
}

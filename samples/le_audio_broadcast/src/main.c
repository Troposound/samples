/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nrfx_clock.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "broadcast.h"

LOG_MODULE_REGISTER(broadcast, LOG_LEVEL_INF);

int main(void)
{
	int err;

	/* LC3 runs on the app core: use 128 MHz instead of the 64 MHz default */
	err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	if (err) {
		LOG_WRN("Could not set 128 MHz (%d)", err);
	}

	err = led_init();
	if (err) {
		LOG_WRN("LED init failed (%d)", err);
	}

	LOG_INF("LE Audio broadcast %s on %s",
		IS_ENABLED(CONFIG_BROADCAST_ROLE_SOURCE) ? "source" : "sink", CONFIG_BOARD);

	err = role_run();
	if (err) {
		LOG_ERR("Stopped (%d)", err);
		led_state_set(LED_STATE_ERROR);
	}

	return 0;
}

/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <nrfx_clock.h>
#include <troposound/volume.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "usb_broadcast.h"

LOG_MODULE_REGISTER(usb_broadcast, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec on_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

#if DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)
/* Nothing plays locally: keep the R1 speaker amplifier off. */
static const struct gpio_dt_spec amp_en = GPIO_DT_SPEC_GET(USER_NODE, amp_enable_gpios);
#endif

int main(void)
{
	int err;

#if DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)
	(void)gpio_pin_configure_dt(&amp_en, GPIO_OUTPUT_INACTIVE);
#endif

	/* LC3 runs on the app core: use 128 MHz instead of the 64 MHz default. */
	err = nrfx_clock_divider_set(NRF_CLOCK_DOMAIN_HFCLK, NRF_CLOCK_HFCLK_DIV_1);
	if (err) {
		LOG_WRN("Could not set 128 MHz (%d)", err);
	}

	LOG_INF("USB broadcast on %s", CONFIG_BOARD_TARGET);

	err = broadcast_start();
	if (err) {
		LOG_ERR("Broadcast start failed (%d)", err);
		return err;
	}

	/* The H1 encoder starts at unity gain; the R1 knob sets it from its position. */
	err = volume_start_cb(usb_volume_set, 0);
	if (err) {
		return err;
	}

	err = usb_start();
	if (err) {
		return err;
	}

	(void)gpio_pin_configure_dt(&on_led, GPIO_OUTPUT_ACTIVE);

	while (true) {
		k_sleep(K_SECONDS(10));
		usb_stats_log();
	}
}

/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "broadcast.h"

#define BLINK_PERIOD K_MSEC(250)

static const struct gpio_dt_spec led_red = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static enum led_state cur_state;
static bool muted;
static bool blink_on;

static void blink_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(blink_work, blink_handler);

static void blink_handler(struct k_work *work)
{
	if (cur_state != LED_STATE_SEARCHING) {
		return;
	}

	blink_on = !blink_on;
	gpio_pin_set_dt(&led_blue, blink_on);
	k_work_reschedule(&blink_work, BLINK_PERIOD);
}

int led_init(void)
{
	const struct gpio_dt_spec *leds[] = {&led_red, &led_green, &led_blue};

	for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
		if (!gpio_is_ready_dt(leds[i])) {
			return -ENODEV;
		}

		int err = gpio_pin_configure_dt(leds[i], GPIO_OUTPUT_INACTIVE);

		if (err) {
			return err;
		}
	}

	return 0;
}

/* Red + green = yellow: streaming while muted. */
static void red_green_update(void)
{
	bool streaming = cur_state == LED_STATE_STREAMING;

	gpio_pin_set_dt(&led_red, cur_state == LED_STATE_ERROR || (streaming && muted));
	gpio_pin_set_dt(&led_green, streaming);
}

void led_muted_set(bool mute)
{
	muted = mute;
	red_green_update();
}

void led_state_set(enum led_state state)
{
	if (state == cur_state) {
		return;
	}

	cur_state = state;
	k_work_cancel_delayable(&blink_work);
	blink_on = false;

	red_green_update();
	gpio_pin_set_dt(&led_blue, 0);

	if (state == LED_STATE_SEARCHING) {
		k_work_reschedule(&blink_work, K_NO_WAIT);
	}
}

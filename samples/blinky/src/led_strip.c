/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(strip, LOG_LEVEL_INF);

#define STRIP_NODE      DT_ALIAS(led_strip)
#define STRIP_LEN       DT_PROP(STRIP_NODE, chain_length)
#define SYMBOL_BITS     DT_PROP(STRIP_NODE, bits_per_symbol)
#define SYMBOL_ONE      DT_PROP(STRIP_NODE, spi_one_frame)
#define SYMBOL_ZERO     DT_PROP(STRIP_NODE, spi_zero_frame)
#define SPI_HZ          DT_PROP(STRIP_NODE, spi_max_frequency)
#define PREAMBLE_BYTES  DIV_ROUND_UP(DT_PROP(STRIP_NODE, reset_delay) * (SPI_HZ / 1000000), 8)
#define FRAME_BYTES     (PREAMBLE_BYTES + DIV_ROUND_UP(STRIP_LEN * 24 * SYMBOL_BITS, 8) + 1)

#define FRAME_MS        20
#define MODE_MS         8000
#define VLED_SETTLE_MS  20
#define FAULT_RETRY_MS  2000
#define MAX_BRIGHT      40

BUILD_ASSERT(STRIP_LEN >= 2, "effects need at least two pixels");

static const struct spi_dt_spec strip =
	SPI_DT_SPEC_GET(STRIP_NODE, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8));
static uint8_t frame_buf[FRAME_BYTES];
static const struct gpio_dt_spec extled_en =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), extled_en_gpios);
static const struct gpio_dt_spec extled_fault =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), extled_fault_gpios);

static struct led_rgb pixels[STRIP_LEN];
static struct gpio_callback fault_cb;
static atomic_t fault_seen;

static void fault_isr(const struct device *port, struct gpio_callback *cb,
		      gpio_port_pins_t pins)
{
	atomic_set(&fault_seen, 1);
}

static uint32_t rnd(void)
{
	static uint32_t state = 0x2545f491;

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

static int strip_show(void)
{
	size_t bit = PREAMBLE_BYTES * 8;
	const struct spi_buf buf = { .buf = frame_buf, .len = sizeof(frame_buf) };
	const struct spi_buf_set tx = { .buffers = &buf, .count = 1 };

	memset(frame_buf, 0, sizeof(frame_buf));
	for (int i = 0; i < STRIP_LEN; i++) {
		const uint8_t grb[3] = { pixels[i].g, pixels[i].r, pixels[i].b };

		for (int c = 0; c < 3; c++) {
			for (int b = 7; b >= 0; b--) {
				uint8_t sym = (grb[c] & BIT(b)) ? SYMBOL_ONE : SYMBOL_ZERO;

				for (int s = SYMBOL_BITS - 1; s >= 0; s--, bit++) {
					if (sym & BIT(s)) {
						frame_buf[bit / 8] |= 0x80 >> (bit % 8);
					}
				}
			}
		}
	}

	return spi_write_dt(&strip, &tx);
}

static struct led_rgb hsv(uint8_t h, uint8_t v)
{
	uint8_t region = h / 43;
	uint8_t rem = (h - region * 43) * 6;
	uint8_t q = (v * (255 - rem)) >> 8;
	uint8_t t = (v * rem) >> 8;

	switch (region) {
	case 0:
		return (struct led_rgb){ .r = v, .g = t, .b = 0 };
	case 1:
		return (struct led_rgb){ .r = q, .g = v, .b = 0 };
	case 2:
		return (struct led_rgb){ .r = 0, .g = v, .b = t };
	case 3:
		return (struct led_rgb){ .r = 0, .g = q, .b = v };
	case 4:
		return (struct led_rgb){ .r = t, .g = 0, .b = v };
	default:
		return (struct led_rgb){ .r = v, .g = 0, .b = q };
	}
}

static void fade_all(uint8_t keep_of_256)
{
	for (int i = 0; i < STRIP_LEN; i++) {
		pixels[i].r = (pixels[i].r * keep_of_256) >> 8;
		pixels[i].g = (pixels[i].g * keep_of_256) >> 8;
		pixels[i].b = (pixels[i].b * keep_of_256) >> 8;
	}
}

static void fx_rainbow(uint32_t frame)
{
	for (int i = 0; i < STRIP_LEN; i++) {
		pixels[i] = hsv(frame * 2 + i * 256 / STRIP_LEN, MAX_BRIGHT);
	}
}

static void fx_scanner(uint32_t frame)
{
	uint32_t period = 2 * (STRIP_LEN - 1);
	uint32_t p = (frame / 2) % period;
	uint32_t pos = p < STRIP_LEN ? p : period - p;

	fade_all(180);
	pixels[pos] = (struct led_rgb){ .r = MAX_BRIGHT };
}

static void fx_police(uint32_t frame)
{
	uint32_t phase = (frame / 4) % 8;

	memset(pixels, 0, sizeof(pixels));
	if (phase == 0 || phase == 2) {
		for (int i = 0; i < STRIP_LEN / 2; i++) {
			pixels[i].r = MAX_BRIGHT;
		}
	} else if (phase == 4 || phase == 6) {
		for (int i = STRIP_LEN / 2; i < STRIP_LEN; i++) {
			pixels[i].b = MAX_BRIGHT;
		}
	}
}

static void fx_disco(uint32_t frame)
{
	fade_all(220);
	if (frame % 2 == 0) {
		pixels[rnd() % STRIP_LEN] = hsv(rnd(), MAX_BRIGHT);
	}
}

static void fx_breathe(uint32_t frame)
{
	uint32_t t = frame % 100;
	uint8_t v = (t < 50 ? t : 100 - t) * MAX_BRIGHT / 50;
	struct led_rgb c = hsv(frame / 4, v);

	for (int i = 0; i < STRIP_LEN; i++) {
		pixels[i] = c;
	}
}

static const struct {
	const char *name;
	void (*fn)(uint32_t frame);
} modes[] = {
	{ "rainbow", fx_rainbow },
	{ "scanner", fx_scanner },
	{ "police", fx_police },
	{ "disco", fx_disco },
	{ "breathe", fx_breathe },
};

static void extled_power(bool on)
{
	if (on) {
		gpio_pin_set_dt(&extled_en, 1);
		k_msleep(VLED_SETTLE_MS);
		atomic_set(&fault_seen, gpio_pin_get_dt(&extled_fault) > 0);
		gpio_pin_interrupt_configure_dt(&extled_fault, GPIO_INT_EDGE_TO_ACTIVE);
		LOG_INF("VLED on, fault %s", atomic_get(&fault_seen) ? "ACTIVE" : "ok");
	} else {
		gpio_pin_interrupt_configure_dt(&extled_fault, GPIO_INT_DISABLE);
		gpio_pin_set_dt(&extled_en, 0);
		LOG_INF("VLED off");
	}
}

static void strip_thread(void *p1, void *p2, void *p3)
{
	int ret;
	int mode = -1;
	uint32_t frame = 0;

	LOG_INF("%d pixels, brightness cap %d/255", STRIP_LEN, MAX_BRIGHT);

	if (!spi_is_ready_dt(&strip)) {
		LOG_ERR("SPI bus %s not ready", strip.bus->name);
		return;
	}

	if (!gpio_is_ready_dt(&extled_en) || !gpio_is_ready_dt(&extled_fault)) {
		LOG_ERR("GPIO not ready");
		return;
	}

	ret = gpio_pin_configure_dt(&extled_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("VLED enable configure failed (%d)", ret);
		return;
	}

	ret = gpio_pin_configure_dt(&extled_fault, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("VLED fault configure failed (%d)", ret);
		return;
	}

	gpio_init_callback(&fault_cb, fault_isr, BIT(extled_fault.pin));
	gpio_add_callback(extled_fault.port, &fault_cb);

	extled_power(true);

	while (1) {
		int64_t now = k_uptime_get();
		int new_mode = (now / MODE_MS) % ARRAY_SIZE(modes);

		if (atomic_cas(&fault_seen, 1, 0)) {
			LOG_ERR("VLED overcurrent, retrying in %d ms", FAULT_RETRY_MS);
			extled_power(false);
			k_msleep(FAULT_RETRY_MS);
			extled_power(true);
			continue;
		}

		if (new_mode != mode) {
			mode = new_mode;
			memset(pixels, 0, sizeof(pixels));
			LOG_INF("Mode %d/%d '%s'", mode + 1, (int)ARRAY_SIZE(modes),
				modes[mode].name);
		}

		modes[mode].fn(frame);
		ret = strip_show();
		if (ret < 0) {
			LOG_ERR("strip_show failed (%d)", ret);
		}
		frame++;

		k_msleep(FRAME_MS);
	}
}

K_THREAD_DEFINE(led_strip_tid, 1024, strip_thread, NULL, NULL, NULL, 7, 0, 0);

/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/audio/codec.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include <troposound/audio_i2s.h>

#include "hw_test.h"

#define USER_NODE   DT_PATH(zephyr_user)
#define CODEC_NODE  DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tac5211)
#define TONE_PERIOD (AUDIO_I2S_SAMPLE_RATE_HZ / 1000) /* 1 kHz */
#define TONE_AMPL   8192.0f                           /* -12 dBFS */

BUILD_ASSERT(AUDIO_I2S_BLOCK_WORDS % TONE_PERIOD == 0, "block must hold whole 1 kHz periods");

#if DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)
static const struct gpio_dt_spec amp_en = GPIO_DT_SPEC_GET(USER_NODE, amp_enable_gpios);
#endif

static const struct device *const codec = DEVICE_DT_GET(CODEC_NODE);

static uint32_t tx_buf[2][AUDIO_I2S_BLOCK_WORDS];
static uint32_t rx_buf[2][AUDIO_I2S_BLOCK_WORDS];
static int16_t tone[TONE_PERIOD];
static float goertzel_coeff;
static atomic_t mode = ATOMIC_INIT(AUDIO_SILENT);
static bool running;

/* Written from the I2S ISR */
static struct k_spinlock lock;
static uint32_t acc_blocks;
static uint32_t acc_peak;
static uint32_t acc_changes;
static double acc_energy;
static double acc_tone_power;

static int16_t sat16(int32_t v)
{
	return (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
}

/* Mic is on one slot, the other is silent; summing covers either */
static int16_t mic_sample(uint32_t word)
{
	return sat16((int16_t)(word & 0xFFFF) + (int16_t)(word >> 16));
}

static void measure_block(const uint32_t *rx)
{
	float s1 = 0.0f;
	float s2 = 0.0f;
	float energy = 0.0f;
	uint32_t peak = 0U;
	uint32_t changes = 0U;
	k_spinlock_key_t key;

	for (size_t i = 0; i < AUDIO_I2S_BLOCK_WORDS; i++) {
		int16_t m = mic_sample(rx[i]);
		float s0 = (float)m + goertzel_coeff * s1 - s2;

		s2 = s1;
		s1 = s0;
		energy += (float)m * m;
		peak = MAX(peak, (uint32_t)abs(m));
		changes += (i > 0 && rx[i] != rx[i - 1]);
	}

	key = k_spin_lock(&lock);
	acc_blocks++;
	acc_peak = MAX(acc_peak, peak);
	acc_changes += changes;
	acc_energy += (double)energy;
	acc_tone_power += (double)(s1 * s1 + s2 * s2 - goertzel_coeff * s1 * s2);
	k_spin_unlock(&lock, key);
}

static void fill_block(uint32_t *tx, const uint32_t *rx)
{
	static size_t phase;
	enum audio_mode m = (enum audio_mode)atomic_get(&mode);

	for (size_t i = 0; i < AUDIO_I2S_BLOCK_WORDS; i++) {
		int16_t s = 0;

		if (m == AUDIO_TONE) {
			s = tone[phase];
		} else if (m == AUDIO_LOOPBACK && rx != NULL) {
			s = mic_sample(rx[i]);
		}
		phase = (phase + 1) % TONE_PERIOD;
		tx[i] = ((uint32_t)(uint16_t)s << 16) | (uint16_t)s;
	}
}

static void i2s_cb(uint32_t *released_rx, const uint32_t *released_tx)
{
	uint32_t *tx = (uint32_t *)released_tx;
	uint32_t *rx = released_rx;

	if (rx == NULL || tx == NULL) {
		/* First request after start */
		tx = tx_buf[1];
		rx = rx_buf[1];
		fill_block(tx, NULL);
	} else {
		measure_block(rx);
		fill_block(tx, rx);
	}

	(void)audio_i2s_set_next_buf(tx, rx);
}

int hw_audio_start(int volume_half_db)
{
	struct audio_codec_cfg cfg = {
		.dai_type = AUDIO_DAI_TYPE_I2S,
		.dai_route = AUDIO_ROUTE_PLAYBACK_CAPTURE,
		.dai_cfg.i2s = {
			.word_size = 16,
			.channels = 2,
			.format = I2S_FMT_DATA_FORMAT_I2S,
			.frame_clk_freq = AUDIO_I2S_SAMPLE_RATE_HZ,
		},
	};
	audio_property_value_t vol = { .vol = volume_half_db };
	int err;

	if (running) {
		return -EALREADY;
	}

	if (!device_is_ready(codec)) {
		return -ENODEV;
	}

	for (size_t i = 0; i < TONE_PERIOD; i++) {
		tone[i] = (int16_t)(TONE_AMPL * sinf(2.0f * 3.14159265f * i / TONE_PERIOD));
	}
	goertzel_coeff = 2.0f * cosf(2.0f * 3.14159265f *
				     (AUDIO_I2S_BLOCK_WORDS / TONE_PERIOD) / AUDIO_I2S_BLOCK_WORDS);

	err = audio_i2s_init();
	if (err) {
		return err;
	}

	atomic_set(&mode, AUDIO_SILENT);
	audio_i2s_callback_set(i2s_cb);
	fill_block(tx_buf[0], NULL);
	err = audio_i2s_start(tx_buf[0], rx_buf[0]);
	if (err) {
		audio_i2s_uninit();
		return err;
	}

	/* Codec PLL locks to BCLK, so start I2S first */
	err = audio_codec_configure(codec, &cfg);
	if (err == 0) {
		err = audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_VOLUME,
					       AUDIO_CHANNEL_ALL, vol);
	}
	if (err) {
		audio_i2s_uninit();
		return err;
	}

#if DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)
	gpio_pin_configure_dt(&amp_en, GPIO_OUTPUT_ACTIVE);
#endif

	(void)audio_i2s_late_count_reset();
	running = true;
	/* ADC high-pass settles in about 1 s at +30 dB gain */
	k_msleep(1200);

	return 0;
}

void hw_audio_stop(void)
{
	if (!running) {
		return;
	}

	atomic_set(&mode, AUDIO_SILENT);
	k_msleep(50);
	audio_i2s_uninit();
#if DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)
	gpio_pin_set_dt(&amp_en, 0);
#endif
	running = false;
}

void hw_audio_mode(enum audio_mode m)
{
	atomic_set(&mode, m);
}

static int to_dbfs_x10(double amplitude)
{
	if (amplitude < 1.0) {
		return -999;
	}

	return (int)lround(200.0 * log10(amplitude / 32768.0));
}

void hw_audio_measure(int ms, struct audio_level *out)
{
	k_spinlock_key_t key;
	uint32_t blocks;

	k_msleep(50); /* skip the block in flight */

	key = k_spin_lock(&lock);
	acc_blocks = 0U;
	acc_peak = 0U;
	acc_changes = 0U;
	acc_energy = 0.0;
	acc_tone_power = 0.0;
	k_spin_unlock(&lock, key);

	k_msleep(ms);

	key = k_spin_lock(&lock);
	blocks = acc_blocks;
	out->blocks = blocks;
	out->peak = acc_peak;
	out->changes = acc_changes;
	if (blocks > 0U) {
		double n = (double)blocks * AUDIO_I2S_BLOCK_WORDS;
		double tone_power = acc_tone_power / blocks;

		out->rms_dbfs_x10 = to_dbfs_x10(sqrt(acc_energy / n));
		/* Goertzel power is (A * N / 2)^2; convert to RMS */
		out->tone_dbfs_x10 = to_dbfs_x10(2.0 * sqrt(MAX(tone_power, 0.0)) /
						 AUDIO_I2S_BLOCK_WORDS / sqrt(2.0));
	} else {
		out->rms_dbfs_x10 = -999;
		out->tone_dbfs_x10 = -999;
	}
	k_spin_unlock(&lock, key);
}

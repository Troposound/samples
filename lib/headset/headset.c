/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdlib.h>

#include <troposound/headset.h>
#include <troposound/tac5211.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(headset, LOG_LEVEL_INF);

#define USER_NODE DT_PATH(zephyr_user)
#define JACK_NODE DT_ALIAS(jack_detect)
#define HAS_JACK  DT_NODE_EXISTS(JACK_NODE)
#define HAS_AMP   DT_NODE_HAS_PROP(USER_NODE, amp_enable_gpios)

static const struct device *codec;
static struct k_spinlock lock;
static bool playing;
static bool inserted;

#if HAS_AMP
static const struct gpio_dt_spec amp_en = GPIO_DT_SPEC_GET(USER_NODE, amp_enable_gpios);
#endif

static void speaker_update(void)
{
	bool on = playing && !inserted;

#if HAS_AMP
	(void)gpio_pin_set_dt(&amp_en, on);
#else
	ARG_UNUSED(on);
#endif
}

void headset_speaker_set(bool on)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	playing = on;
	speaker_update();
	k_spin_unlock(&lock, key);
}

#if HAS_JACK
static const struct gpio_dt_spec jack = GPIO_DT_SPEC_GET(JACK_NODE, gpios);

/* Linear to -9 dBFS, 1:4 above, ceiling at -1 dBFS. */
#define CLIP_KNEE  11627
#define CLIP_CEIL  29205
#define CLIP_SLOPE 4

static int32_t mic_gain_q8;

static int16_t soft_clip(int32_t v)
{
	int32_t a = abs(v);

	if (a <= CLIP_KNEE) {
		return v;
	}

	a = MIN(CLIP_KNEE + (a - CLIP_KNEE) / CLIP_SLOPE, CLIP_CEIL);
	return v < 0 ? -a : a;
}

static void jack_set(bool in)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err;

	inserted = in;
	speaker_update();
	k_spin_unlock(&lock, key);

	err = tac5211_capture_input_set(codec, in ? TAC5211_CAPTURE_INPUT_IN2P
						  : TAC5211_CAPTURE_INPUT_IN1P);
	if (err == 0) {
		err = tac5211_playback_eq_enable(codec, !in);
	}

	LOG_INF("%s%s", in ? "Headset in: headset mic, speaker off" : "Headset out: onboard mic, speaker",
		err ? " (codec error)" : "");
}

static void jack_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (codec != NULL && evt->type == INPUT_EV_KEY &&
	    evt->code == DT_PROP(JACK_NODE, zephyr_code)) {
		jack_set(evt->value);
	}
}

INPUT_CALLBACK_DEFINE(NULL, jack_cb, NULL);
#endif /* HAS_JACK */

void headset_mic_gain(int16_t *pcm, size_t n, size_t stride)
{
#if HAS_JACK
	if (!inserted) {
		return;
	}

	for (size_t i = 0; i < n * stride; i += stride) {
		pcm[i] = soft_clip((pcm[i] * mic_gain_q8) >> 8);
	}
#else
	ARG_UNUSED(pcm);
	ARG_UNUSED(n);
	ARG_UNUSED(stride);
#endif
}

int headset_start(const struct device *dev)
{
#if HAS_AMP
	int err = gpio_pin_configure_dt(&amp_en, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("Amp enable GPIO failed (%d)", err);
		return err;
	}
#endif

	codec = dev;

#if HAS_JACK
	mic_gain_q8 = lroundf(256.0f * powf(10.0f, CONFIG_TROPOSOUND_HEADSET_MIC_GAIN_DB / 20.0f));

	/* gpio-keys only reports changes. */
	jack_set(gpio_pin_get_dt(&jack) == 1);
#endif

	return 0;
}

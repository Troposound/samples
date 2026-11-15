/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Mute button (R1 PTT, H1 mute), headset mode (R1) and, for the sink,
 * volume from the R1 knob or the H1 rotary encoder and the LED strip effect
 * from the H1 smart button.
 */

#include <troposound/headset.h>
#include <troposound/volume.h>
#include <zephyr/audio/codec.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "broadcast.h"

LOG_MODULE_REGISTER(controls, LOG_LEVEL_INF);

static const struct device *codec;
static atomic_t mute_state;

bool controls_muted(void)
{
	return atomic_get(&mute_state);
}

static void mute_toggle(void)
{
	bool muted = !atomic_xor(&mute_state, 1);

	if (IS_ENABLED(CONFIG_BROADCAST_ROLE_SINK)) {
		audio_codec_set_property(codec, AUDIO_PROPERTY_OUTPUT_MUTE, AUDIO_CHANNEL_ALL,
					 (audio_property_value_t){.mute = muted});
		audio_codec_apply_properties(codec);
	}

	led_muted_set(muted);
	LOG_INF("%s", muted ? "Muted" : "Unmuted");
}

static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (codec == NULL || evt->type != INPUT_EV_KEY) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_0:    /* R1 PTT */
	case INPUT_KEY_MUTE: /* H1 mute */
		if (evt->value) {
			mute_toggle();
		}
		break;
	case INPUT_KEY_PLAYPAUSE: /* H1 smart button */
		if (evt->value) {
			strip_mode_next();
		}
		break;
	default:
		break;
	}
}

INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);

void controls_start(const struct device *dev)
{
	codec = dev;

	(void)headset_start(codec);

#if IS_ENABLED(CONFIG_BROADCAST_ROLE_SINK)
	(void)volume_start(codec, CONFIG_BROADCAST_VOLUME_HALF_DB);
#endif
}

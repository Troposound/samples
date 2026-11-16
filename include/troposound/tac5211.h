/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_TAC5211_H_
#define TROPOSOUND_TAC5211_H_

#include <stdbool.h>

#include <zephyr/device.h>

enum tac5211_capture_input {
	TAC5211_CAPTURE_INPUT_IN1P,
	TAC5211_CAPTURE_INPUT_IN2P,
};

int tac5211_capture_input_set(const struct device *dev, enum tac5211_capture_input input);

enum tac5211_playback_eq_preset {
	TAC5211_PLAYBACK_EQ_PRESET_FLAT,
	TAC5211_PLAYBACK_EQ_PRESET_VOICE,
	TAC5211_PLAYBACK_EQ_PRESET_BRIGHT,
	TAC5211_PLAYBACK_EQ_PRESET_WARM,
	TAC5211_PLAYBACK_EQ_PRESET_BASS_BOOST,
};

/* Speaker EQ or allpass. May be called before audio_codec_configure(). */
int tac5211_playback_eq_enable(const struct device *dev, bool enable);

/* Speaker voicing, applied while the speaker EQ is enabled */
int tac5211_playback_eq_preset_set(const struct device *dev,
				   enum tac5211_playback_eq_preset preset);

#endif /* TROPOSOUND_TAC5211_H_ */

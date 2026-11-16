/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_HEADSET_H_
#define TROPOSOUND_HEADSET_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>

/* R1 jack: headset mic (IN2P), EQ bypass, speaker amp off. Call after I2S clocks start. */
int headset_start(const struct device *codec);

/* Speaker amplifier on while playing, unless a headset is plugged in */
void headset_speaker_set(bool playing);

/* Gain + soft clip on every stride-th sample, in place. No-op unless headset mic is active. */
void headset_mic_gain(int16_t *pcm, size_t n, size_t stride);

#endif /* TROPOSOUND_HEADSET_H_ */

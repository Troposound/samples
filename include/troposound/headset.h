/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_HEADSET_H_
#define TROPOSOUND_HEADSET_H_

#include <stdbool.h>

#include <zephyr/device.h>

/* Headset mode on the R1 jack. With a TRRS headset plugged in, capture uses
 * the headset mic (IN2P), the speaker EQ is bypassed and the speaker
 * amplifier stays off. Start once the I2S clocks run.
 */
int headset_start(const struct device *codec);

/* Speaker amplifier on while playing, unless a headset is plugged in. */
void headset_speaker_set(bool playing);

#endif /* TROPOSOUND_HEADSET_H_ */

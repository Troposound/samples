/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BROADCAST_H_
#define BROADCAST_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/sys/util.h>

enum led_state {
	LED_STATE_OFF,
	LED_STATE_SEARCHING, /* blue blinking: advertising (source) / scanning (sink) */
	LED_STATE_STREAMING, /* solid green, yellow while muted */
	LED_STATE_ERROR,     /* solid red */
};

int led_init(void);
void led_state_set(enum led_state state);
void led_muted_set(bool muted);

/* Buttons, volume knob/encoder and jack detect. Call once the codec runs. */
void controls_start(const struct device *codec);
bool controls_muted(void);

int role_run(void);

#define STRIP_FRAME_SAMPLES 480 /* 10 ms of 48 kHz stereo, left in the low half-word */

#if defined(CONFIG_BROADCAST_LED_STRIP)
void strip_audio_feed(const uint32_t *pcm);
void strip_mode_next(void);
#else
static inline void strip_audio_feed(const uint32_t *pcm)
{
	ARG_UNUSED(pcm);
}

static inline void strip_mode_next(void)
{
}
#endif

#endif /* BROADCAST_H_ */

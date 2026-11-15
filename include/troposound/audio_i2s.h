/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_AUDIO_I2S_H_
#define TROPOSOUND_AUDIO_I2S_H_

#include <stdint.h>

#define AUDIO_I2S_SAMPLE_RATE_HZ 48000
#define AUDIO_I2S_BLOCK_WORDS    CONFIG_TROPOSOUND_AUDIO_I2S_BLOCK_WORDS

/* One word = one stereo frame: left in the low half-word, right in the high.
 * Both pointers are NULL on the first request after start and after a late
 * hand-off, when nrfx reused the pair in flight.
 */
typedef void (*audio_i2s_callback_t)(uint32_t *released_rx, const uint32_t *released_tx);

int audio_i2s_init(void);
void audio_i2s_callback_set(audio_i2s_callback_t callback);
int audio_i2s_start(const uint32_t *tx_buf, uint32_t *rx_buf);
int audio_i2s_set_next_buf(const uint32_t *tx_buf, uint32_t *rx_buf);
void audio_i2s_stop(void);
void audio_i2s_uninit(void);

/* Times nrfx reused the in-flight pair because the next one came late. */
uint32_t audio_i2s_late_count_reset(void);

#endif /* TROPOSOUND_AUDIO_I2S_H_ */

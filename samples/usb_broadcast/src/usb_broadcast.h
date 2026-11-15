/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef USB_BROADCAST_H_
#define USB_BROADCAST_H_

#include <stdint.h>

#define PCM_RATE_HZ   48000
#define FRAME_MS      10
#define FRAME_SAMPLES (PCM_RATE_HZ / 1000 * FRAME_MS)

int usb_start(void);
void usb_volume_set(int half_db);
void usb_stats_log(void);

/* One 10 ms stereo frame of host audio, or silence while there is none.
 * One word per sample pair, left in the low half-word.
 */
void usb_frame_get(uint32_t *pcm);

/* Returns once both BISes are up. */
int broadcast_start(void);

#endif /* USB_BROADCAST_H_ */

/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_VOLUME_H_
#define TROPOSOUND_VOLUME_H_

#include <zephyr/device.h>

/* R1 knob or H1 encoder (1 dB/detent) to codec volume; -60..+6 dB in half-dB steps */
int volume_start(const struct device *codec, int initial_half_db);

typedef void (*volume_set_t)(int half_db);

/* As volume_start(), but reports to `set` instead of a codec */
int volume_start_cb(volume_set_t set, int initial_half_db);

#endif /* TROPOSOUND_VOLUME_H_ */

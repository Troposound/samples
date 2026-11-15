/*
 * Copyright (c) 2026 Troposound
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TROPOSOUND_VOLUME_H_
#define TROPOSOUND_VOLUME_H_

#include <zephyr/device.h>

/* Output volume of `codec` from the R1 volume knob (absolute position) or
 * the H1 rotary encoder (1 dB per detent, starting at initial_half_db).
 * Range -60..+6 dB, in the codec's half-dB steps.
 */
int volume_start(const struct device *codec, int initial_half_db);

#endif /* TROPOSOUND_VOLUME_H_ */

# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0

add_subdirectory_ifdef(CONFIG_TROPOSOUND_AUDIO_I2S audio_i2s)
add_subdirectory_ifdef(CONFIG_TROPOSOUND_HEADSET headset)
add_subdirectory_ifdef(CONFIG_TROPOSOUND_VOLUME volume)

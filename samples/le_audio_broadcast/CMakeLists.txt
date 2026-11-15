# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(le_audio_broadcast)

target_sources(app PRIVATE src/main.c src/led.c src/controls.c)
target_sources_ifdef(CONFIG_BROADCAST_ROLE_SOURCE app PRIVATE src/source.c)
target_sources_ifdef(CONFIG_BROADCAST_ROLE_SINK app PRIVATE src/sink.c)

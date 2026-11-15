# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hw_test)

target_sources(app PRIVATE src/main.c src/tests.c src/audio.c src/radio.c src/battery.c)

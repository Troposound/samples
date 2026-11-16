# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0

# SWD fails to power up the DAP at 4 MHz on H1; 2 MHz is reliable.
if(CONFIG_BOARD_H1_NRF5340_CPUAPP)
  board_runner_args(jlink "--device=nrf5340_xxaa_app" "--speed=2000")
endif()

if(CONFIG_BOARD_H1_NRF5340_CPUNET)
  board_runner_args(jlink "--device=nrf5340_xxaa_net" "--speed=2000")
endif()

include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

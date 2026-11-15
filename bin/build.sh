#!/usr/bin/env bash
# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0
#
# Pristine build of every sample for every board into bin/<board>/<sample>.hex
# (application and network core merged). Run inside the west workspace with
# the nRF Connect SDK toolchain on PATH.
#
#   bin/build.sh [build-root]     (default: <west topdir>/build-bin)

set -euo pipefail

BIN="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$BIN")"
ZEPHYR="$(west topdir)/zephyr"
BUILD_ROOT="${1:-$(west topdir)/build-bin}"

BOARDS="r1 h1"
# <output name>:<sample dir>:<extra cmake args>
VARIANTS="
blinky:blinky:
hw_test:hw_test:
le_audio_broadcast_source:le_audio_broadcast:
le_audio_broadcast_sink:le_audio_broadcast:-DEXTRA_CONF_FILE=sink.conf
usb_audio:usb_audio:
usb_broadcast:usb_broadcast:
"

for board in $BOARDS; do
	mkdir -p "$BIN/$board"
	for v in $VARIANTS; do
		IFS=: read -r name sample extra <<<"$v"
		build="$BUILD_ROOT/${board}_$name"

		echo "=== $board $name"
		# shellcheck disable=SC2086 # $extra is empty or one argument
		west build -p -b "$board/nrf5340/cpuapp" -d "$build" "$REPO/samples/$sample" \
			${extra:+-- $extra}

		app="$(sed -n 's/^default: //p' "$build/domains.yaml")"
		hexes=("$build/$app/zephyr/zephyr.hex")
		if [ -d "$build/ipc_radio" ]; then
			hexes+=("$build/ipc_radio/zephyr/zephyr.hex")
		fi
		python3 "$ZEPHYR/scripts/build/mergehex.py" -o "$BIN/$board/$name.hex" "${hexes[@]}"
	done
done

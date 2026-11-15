#!/usr/bin/env bash
# Copyright (c) 2026 Troposound
# SPDX-License-Identifier: Apache-2.0
#
# Flash a merged nRF5340 image. nrfutil programs one core per call and
# rejects addresses of the other core, so the image is split at 0x01000000:
# network core first (if present), then application core, then reset.
#
#   bin/flash.sh <hex> [J-Link serial number]
#   bin/flash.sh bin/r1/le_audio_broadcast_source.hex 69654557

set -euo pipefail

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
	echo "usage: $0 <merged.hex> [serial]" >&2
	exit 1
fi

sel=()
if [ $# -ge 2 ]; then
	sel=(--serial-number "$2")
fi
opts=(--options chip_erase_mode=ERASE_RANGES_TOUCHED_BY_FIRMWARE)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

python3 - "$1" "$tmp" <<'EOF'
import sys

src, out = sys.argv[1], sys.argv[2]
cores = {"app": [], "net": []}
seg = {"app": None, "net": None}
upper = 0
ela = None

for line in open(src):
	line = line.strip()
	if not line.startswith(":"):
		continue
	rtype = int(line[7:9], 16)
	if rtype == 4:
		upper = int(line[9:13], 16)
		ela = line
	elif rtype == 0:
		core = "net" if upper >= 0x0100 else "app"
		if seg[core] != upper:
			cores[core].append(ela or ":020000040000FA")
			seg[core] = upper
		cores[core].append(line)

for core, lines in cores.items():
	if lines:
		with open(f"{out}/{core}.hex", "w") as f:
			f.write("\n".join(lines + [":00000001FF"]) + "\n")
EOF

if [ -f "$tmp/net.hex" ]; then
	nrfutil device program --firmware "$tmp/net.hex" --core Network "${sel[@]}" "${opts[@]}"
fi
nrfutil device program --firmware "$tmp/app.hex" --core Application "${sel[@]}" "${opts[@]}"
nrfutil device reset "${sel[@]}"

#!/bin/bash
# Records the frame barcode on the main display and prints what each refresh showed (the
# frame-timing skill). With SINELESS_FRAME_BARCODE=1 set for the host, the editor open and the
# transport running:
#
#   scripts/frame-barcode/record.sh [seconds] [out.jsonl]
#
# FRAME_BARCODE_DISPLAY=external (or builtin) records that display rather than the main one.
#
# It saves two frames, finds the strips in them (they must move on between the two), records one
# grey per cell per refresh for the given seconds (60 by default), and analyses the recording. The
# terminal running it needs Screen Recording permission. A minute at 120 Hz is under 10 MB.
set -euo pipefail
cd "$(dirname "$0")/../.."
seconds="${1:-60}"
out="${2:-build/frame-barcode-$(date +%Y%m%d-%H%M%S).jsonl}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$(dirname "$out")"

size="$(swift scripts/frame-barcode/record.swift grab "$work/frame" 2 300)"
width="$(node -e 'console.log(JSON.parse(process.argv[1]).width)' "$size")"
height="$(node -e 'console.log(JSON.parse(process.argv[1]).height)' "$size")"
node scripts/frame-barcode/analyse.ts --locate "$width" "$height" "$work"/frame-*.gray \
    > "$work/places.json"
echo "Strips found: $(node -e 'console.log(JSON.parse(require("fs").readFileSync(process.argv[1])).length)' "$work/places.json"); recording ${seconds} s to $out"
swift scripts/frame-barcode/record.swift record "$work/places.json" "$seconds" "$out"
node scripts/frame-barcode/analyse.ts "$out"

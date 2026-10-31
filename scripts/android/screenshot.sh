#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi

OUTPUT=${1:-"${GUNGNIR_QA_DIR:-$ROOT/build/qa/android}/$(date +%Y%m%d-%H%M%S).png"}
mkdir -p "$(dirname "$OUTPUT")"
"$ADB" "${ADB_ARGS[@]}" exec-out screencap -p > "$OUTPUT"
if [ ! -s "$OUTPUT" ]; then
    echo "screenshot_failed: empty image" >&2
    exit 1
fi
echo "$OUTPUT"

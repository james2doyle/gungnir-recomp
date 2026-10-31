#!/usr/bin/env bash
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
APK=${1:-"$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"}
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi

if [ ! -f "$APK" ]; then
    echo "missing_apk: $APK (run scripts/android/build.sh)" >&2
    exit 1
fi
"$ADB" "${ADB_ARGS[@]}" install -r "$APK"

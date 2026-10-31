#!/usr/bin/env bash
set -euo pipefail

ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi

exec "$ADB" "${ADB_ARGS[@]}" logcat -s gungnir:I SDL:V AndroidRuntime:E

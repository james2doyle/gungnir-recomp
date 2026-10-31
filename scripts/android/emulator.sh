#!/usr/bin/env bash
set -euo pipefail

SDK_DIR=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-/opt/android-sdk}}
AVD=${GUNGNIR_AVD:-gungnir}
PORT=${GUNGNIR_EMULATOR_PORT:-5554}
AVD_HOME=${ANDROID_AVD_HOME:-$HOME/.config/.android/avd}
LOG=${GUNGNIR_EMULATOR_LOG:-/tmp/gungnir-emulator.log}
EMULATOR="$SDK_DIR/emulator/emulator"

if [ ! -x "$EMULATOR" ]; then
    echo "missing_emulator: $EMULATOR" >&2
    exit 1
fi
if [ ! -d "$AVD_HOME" ]; then
    echo "missing_avd_home: $AVD_HOME" >&2
    exit 1
fi

SERIAL="emulator-$PORT"
if adb -s "$SERIAL" get-state >/dev/null 2>&1; then
    echo "emulator already running: $SERIAL"
    exit 0
fi

export ANDROID_AVD_HOME="$AVD_HOME"
"$EMULATOR" -avd "$AVD" -port "$PORT" \
    -no-window -no-audio -no-boot-anim \
    -gpu swiftshader_indirect -no-snapshot -wipe-data \
    -netdelay none -netspeed full >"$LOG" 2>&1 &
EMULATOR_PID=$!
echo "emulator starting: avd=$AVD serial=$SERIAL pid=$EMULATOR_PID log=$LOG"

for _ in $(seq 1 180); do
    if adb -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r' | grep -qx 1; then
        echo "emulator ready: $SERIAL"
        exit 0
    fi
    sleep 1
done

echo "emulator_boot_timeout: see $LOG" >&2
exit 1

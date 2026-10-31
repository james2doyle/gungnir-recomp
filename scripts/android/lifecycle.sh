#!/usr/bin/env bash
set -euo pipefail

ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity

count() {
    "$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I | grep -c "$1" || true
}

wait_for() {
    local pattern=$1
    for _ in $(seq 1 30); do
        if "$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I | grep -q "$pattern"; then
            return 0
        fi
        sleep 0.5
    done
    echo "timeout: $pattern" >&2
    return 1
}

wait_for_more() {
    local pattern=$1
    local previous=$2
    for _ in $(seq 1 30); do
        if [ "$(count "$pattern")" -gt "$previous" ]; then
            return 0
        fi
        sleep 0.5
    done
    echo "timeout: $pattern (waiting for a new occurrence)" >&2
    return 1
}

if "$ADB" "${ADB_ARGS[@]}" shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before lifecycle.sh" >&2
    exit 1
fi

"$ADB" "${ADB_ARGS[@]}" logcat -c
"$ADB" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE"
"$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" >/dev/null
wait_for "first frame"

for _ in 1 2 3; do
    paused=$(count "lifecycle_paused")
    "$ADB" "${ADB_ARGS[@]}" shell input keyevent KEYCODE_HOME
    wait_for_more "lifecycle_paused" "$paused"
    resumed=$(count "lifecycle_resumed")
    "$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" >/dev/null
    wait_for_more "lifecycle_resumed" "$resumed"
done

last=$("$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I | grep -E "first frame|lifecycle_resumed" | tail -1)
dims=$(printf '%s\n' "$last" | grep -oE '[0-9]+x[0-9]+' | tail -1)
width=${dims%x*}
height=${dims#*x}
if [ -z "$dims" ] || [ "$width" -le "$height" ]; then
    echo "orientation_not_landscape: $last" >&2
    exit 1
fi

if "$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I | grep -q "audio_tone_ready"; then
    resumed=$(count "audio_resumed")
    if [ "$resumed" -lt 3 ]; then
        echo "audio_not_resumed: $resumed" >&2
        exit 1
    fi
fi

echo "LIFECYCLE_OK $last"

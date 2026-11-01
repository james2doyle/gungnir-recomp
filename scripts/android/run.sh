#!/usr/bin/env bash
set -euo pipefail

ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity

if "$ADB" "${ADB_ARGS[@]}" shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before run.sh" >&2
    exit 1
fi

"$ADB" "${ADB_ARGS[@]}" logcat -c
"$ADB" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE"
"$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" >/dev/null
for _ in $(seq 1 120); do
    LOG=$("$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I 2>/dev/null || true)
    if printf '%s\n' "$LOG" | grep -q "session_stop.*cause=first_flip.*missing_import=0"; then
        echo "SESSION_OK"
        exit 0
    fi
    if printf '%s\n' "$LOG" | grep -q "session_stop"; then
        echo "session_stop_not_first_flip" >&2
        exit 1
    fi
    if printf '%s\n' "$LOG" | grep -q "first frame"; then
        echo "FRAME_OK"
        exit 0
    fi
    if printf '%s\n' "$LOG" | grep -q "session_start_failed"; then
        echo "session_start_failed" >&2
        exit 1
    fi
    sleep 0.5
done
echo "frame_timeout: no 'session_stop' or 'first frame' log line" >&2
exit 1

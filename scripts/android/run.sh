#!/usr/bin/env bash
set -euo pipefail

ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity
# Automation-only snapshot load: device path pushed beforehand
# (e.g. .../files/snapshots/snapshot.gsnap). Loads before the session
# thread starts; never part of the normal boot.
SNAPSHOT_IN=${SNAPSHOT_IN:-}

if "$ADB" "${ADB_ARGS[@]}" shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before run.sh" >&2
    exit 1
fi

"$ADB" "${ADB_ARGS[@]}" logcat -c
"$ADB" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE"
if [ -n "$SNAPSHOT_IN" ]; then
    "$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" --es snapshot_in "$SNAPSHOT_IN" >/dev/null
else
    "$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" >/dev/null
fi
# A resumed session never prints "first frame"; flowing guest_frame lines
# are its execution proof.
FRAME_PATTERN="first frame"
if [ -n "$SNAPSHOT_IN" ]; then
    FRAME_PATTERN="guest_frame"
fi
if [ -n "$SNAPSHOT_IN" ]; then
    for _ in $(seq 1 180); do
        LOG=$("$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I 2>/dev/null || true)
        if printf '%s\n' "$LOG" | grep -q "session_resume pc="; then
            echo "RESUME_OK"
            break
        fi
        if printf '%s\n' "$LOG" | grep -q "snapshot_in_failed\|session_start_failed"; then
            echo "snapshot_in_failed: $SNAPSHOT_IN" >&2
            exit 1
        fi
        sleep 0.5
    done
    if ! printf '%s\n' "$LOG" | grep -q "session_resume pc="; then
        echo "resume_timeout: no session_resume for $SNAPSHOT_IN" >&2
        exit 1
    fi
    # Drop the replaced session's host_stop line so the frame wait below
    # only sees the resumed session's output.
    "$ADB" "${ADB_ARGS[@]}" logcat -c
fi
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
    if printf '%s\n' "$LOG" | grep -q "$FRAME_PATTERN"; then
        echo "FRAME_OK"
        exit 0
    fi
    if printf '%s\n' "$LOG" | grep -q "session_start_failed"; then
        echo "session_start_failed" >&2
        exit 1
    fi
    sleep 0.5
done
echo "frame_timeout: no 'session_stop', '$FRAME_PATTERN' log line" >&2
exit 1

#!/usr/bin/env bash
set -euo pipefail

# T13i device reproduction: title -> NEW GAME -> difficulty -> ADVANCE ->
# confirm, then watch for the former null-object fault or a process kill.
# Screenshots and logcat land under build/qa/android/ (gitignored).

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi

# Defaults are the M2102K1AC (3200x1440) overlay positions measured for this
# path. Override for another device or a custom touch layout.
TITLE_X=${TITLE_X:-2624}
TITLE_Y=${TITLE_Y:-1056}
DPAD_DOWN_X=${DPAD_DOWN_X:-442}
DPAD_DOWN_Y=${DPAD_DOWN_Y:-1224}
CONFIRM_X=${CONFIRM_X:-2624}
CONFIRM_Y=${CONFIRM_Y:-1056}
HOLD_MS=${HOLD_MS:-250}
TITLE_WAIT=${TITLE_WAIT:-25}
MONITOR_SECONDS=${MONITOR_SECONDS:-24}
MEM_LIMIT_MB=${MEM_LIMIT_MB:-2048}
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$ROOT/build/qa/android/t13i-newgame-$STAMP"
mkdir -p "$OUT"

adb_() { "$ADB" "${ADB_ARGS[@]}" "$@"; }

tap_hold() {
    adb_ shell input swipe "$1" "$2" "$1" "$2" "$3"
}

shot() {
    adb_ exec-out screencap -p > "$OUT/$1.png"
}

wait_for_frame() {
    for _ in $(seq 1 160); do
        if adb_ logcat -d -s gungnir:I 2>/dev/null | grep -q "first frame\|guest_frame="; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

fault_lines() {
    adb_ logcat -d 2>/dev/null | grep -E "Guest memory access|session_run_failed|guest_pc=0x" || true
}

if adb_ shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before running this script" >&2
    exit 2
fi

adb_ get-state >/dev/null
adb_ shell am force-stop "$PACKAGE"
adb_ logcat -c
adb_ shell am start -n "$ACTIVITY" >/dev/null

if ! wait_for_frame; then
    echo "frame_timeout: no first frame within 80s" >&2
    adb_ logcat -d > "$OUT/logcat.txt" 2>/dev/null || true
    exit 2
fi

sleep "$TITLE_WAIT"
shot 1-title
tap_hold "$TITLE_X" "$TITLE_Y" "$HOLD_MS"
sleep 2
shot 2-difficulty
tap_hold "$DPAD_DOWN_X" "$DPAD_DOWN_Y" 150
sleep 1
shot 3-advance-selected
adb_ logcat -c
tap_hold "$CONFIRM_X" "$CONFIRM_Y" "$HOLD_MS"

fault=""
killed=0
growth=0
pss_kb=0
: > "$OUT/meminfo.txt"
for _ in $(seq 1 "$((MONITOR_SECONDS / 2))"); do
    sleep 2
    fault=$(fault_lines)
    if [ -n "$fault" ]; then
        break
    fi
    if ! adb_ shell pidof "$PACKAGE" >/dev/null 2>&1; then
        killed=1
        break
    fi
    if [ ! -f "$OUT/4-prologue.png" ]; then
        shot 4-prologue
    fi
    mem=$(adb_ shell dumpsys meminfo "$PACKAGE" 2>/dev/null || true)
    # Newer Android puts the row in one column group ("Native Heap  <pss> ..."),
    # older builds split it ("Native Heap  <size>  <alloc> ..."). Take the first
    # numeric field after the label in both layouts.
    line=$(printf '%s\n' "$mem" | awk '/Native Heap/ { for (i = 1; i <= NF; ++i) if ($i ~ /^[0-9]+$/) { print $i; exit } }')
    if [[ "$line" =~ ^[0-9]+$ ]]; then
        pss_kb=$line
        echo "$(date +%H:%M:%S) native_heap_pss_kb=$pss_kb" >> "$OUT/meminfo.txt"
        if [ "$pss_kb" -gt "$((MEM_LIMIT_MB * 1024))" ]; then
            growth=1
            break
        fi
    fi
done
shot 5-after-monitor
adb_ logcat -d > "$OUT/logcat.txt" 2>/dev/null || true
adb_ shell dumpsys meminfo "$PACKAGE" > "$OUT/meminfo-final.txt" 2>/dev/null || true
adb_ shell dumpsys activity exit-info "$PACKAGE" > "$OUT/exit-info.txt" 2>/dev/null || true

if [ -n "$fault" ]; then
    echo "GUEST_FAULT"
    printf '%s\n' "$fault" | tail -5
    echo "evidence: $OUT"
    exit 1
fi

if [ "$killed" -eq 1 ]; then
    reason=$(grep -m1 "reason=" "$OUT/exit-info.txt" || true)
    echo "PROCESS_KILLED ${reason:-reason=unknown}"
    echo "evidence: $OUT"
    exit 3
fi

if [ "$growth" -eq 1 ]; then
    echo "PROCESS_MEMORY_GROWTH native_heap_pss_mb=$((pss_kb / 1024)) limit_mb=$MEM_LIMIT_MB"
    echo "evidence: $OUT"
    exit 4
fi

echo "NEW_GAME_REACHED native_heap_pss_mb=$((pss_kb / 1024))"
echo "evidence: $OUT"
exit 0

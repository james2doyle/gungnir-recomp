#!/usr/bin/env bash
# T15e device QA: host snapshot -> adb push -> Menu load -> play -> menu
# save/restore -> cold-boot restores -> corrupt-file named error.
# Screenshots and logcat land under build/qa/android/ (gitignored).
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi
SNAPSHOT_HOST=${SNAPSHOT_HOST:-${TMPDIR:-/tmp}/gungnir-temp/snap/battle/battleP1.gsnap}
SNAPSHOT_DEV=/sdcard/Android/data/com.gungnir.recomp/files/snapshots/snapshot.gsnap
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$ROOT/build/qa/android/t15e-snapshot-$STAMP"
mkdir -p "$OUT"

adb_() { "$ADB" "${ADB_ARGS[@]}" "$@"; }

fail() { echo "qa-snapshot FAIL: $1" >&2; adb_ logcat -d > "$OUT/logcat-fail.txt" 2>/dev/null || true; exit 1; }

tap_text() {
    local text="$1"
    local attempt=0
    while [ "$attempt" -lt 2 ]; do
        adb_ shell uiautomator dump /sdcard/uidump.xml >/dev/null 2>&1
        local xml
        xml=$(adb_ shell cat /sdcard/uidump.xml 2>/dev/null)
        local bounds
        bounds=$(printf '%s' "$xml" | grep -o "text=\"$text\"[^>]*bounds=\"[^\"]*\"" | head -n 1 | grep -o "bounds=\"[^\"]*\"" | tr -d 'bounds="') || true
        if [ -n "$bounds" ]; then
            local x1y1x2y2
            x1y1x2y2=$(printf '%s' "$bounds" | tr '][' ' ' | tr ',' ' ')
            set -- $x1y1x2y2
            adb_ shell input tap $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
            return 0
        fi
        # Row may be below the fold in the scrollable menu: swipe up inside the
        # panel (anchored on a visible row, display coords vary by orientation)
        # and re-dump once before giving up.
        local anchor
        anchor=$(printf '%s' "$xml" | grep -o "text=\"Save state\"[^>]*bounds=\"[^\"]*\"" | head -n 1 | grep -o "bounds=\"[^\"]*\"" | tr -d 'bounds="') || true
        [ -n "$anchor" ] || anchor=$(printf '%s' "$xml" | grep -o "text=\"Settings\"[^>]*bounds=\"[^\"]*\"" | head -n 1 | grep -o "bounds=\"[^\"]*\"" | tr -d 'bounds="') || true
        if [ -n "$anchor" ]; then
            local coords
            coords=$(printf '%s' "$anchor" | tr '][' ' ' | tr ',' ' ')
            set -- $coords
            adb_ shell input swipe $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 - 500 )) 400
            sleep 1
        fi
        attempt=$((attempt + 1))
    done
    fail "ui text not found: $text"
}

wait_log() {
    local pattern="$1" tries="${2:-120}"
    for _ in $(seq 1 "$tries"); do
        if adb_ logcat -d 2>/dev/null | grep -q "$pattern"; then
            return 0
        fi
        sleep 1
    done
    return 1
}

shot() { adb_ exec-out screencap -p > "$OUT/$1.png"; }

echo "out=$OUT"
[ -f "$SNAPSHOT_HOST" ] || fail "host snapshot missing: $SNAPSHOT_HOST"

if adb_ shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before running this script" >&2
    exit 2
fi

# --- install + push ---------------------------------------------------------
adb_ install -r "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" >&2
adb_ shell mkdir -p "$(dirname "$SNAPSHOT_DEV")"
adb_ push "$SNAPSHOT_HOST" "$SNAPSHOT_DEV" >&2
adb_ shell ls -l "$SNAPSHOT_DEV"

# --- cold boot 1: intent load ------------------------------------------------
adb_ shell am force-stop "$PACKAGE"
adb_ logcat -c
adb_ shell am start -n "$ACTIVITY" --es snapshot_in "$SNAPSHOT_DEV" >/dev/null
wait_log "session_snapshot_in=$SNAPSHOT_DEV" 90 || fail "intent snapshot not loaded"
wait_log "session_resume pc=" 90 || fail "session did not resume from snapshot"
shot 1-resumed
sleep 60
shot 2-after-60s

# --- menu save ----------------------------------------------------------------
tap_text "Menu"
sleep 1
tap_text "Save state"
wait_log "snapshot_saved path=" 60 || fail "menu save did not log snapshot_saved"
shot 3-after-save

# --- menu load ------------------------------------------------------------------
tap_text "Menu"
sleep 1
tap_text "Load state"
wait_log "snapshot_loaded path=" 60 || fail "menu load did not log snapshot_loaded"
wait_log "session_resume pc=" 60 || fail "session did not resume after menu load"
sleep 5
shot 4-after-load

# --- cold boots 2 and 3 ---------------------------------------------------------
for i in 2 3; do
    adb_ shell am force-stop "$PACKAGE"
    sleep 2
    adb_ logcat -c
    adb_ shell am start -n "$ACTIVITY" --es snapshot_in "$SNAPSHOT_DEV" >/dev/null
    wait_log "session_resume pc=" 90 || fail "cold boot $i did not resume"
    sleep 5
    shot "5-coldboot-$i"
done

# --- corrupt file: named error, session stops ------------------------------------
printf 'GSNP-corrupt' | adb_ shell "cat > $SNAPSHOT_DEV"
adb_ shell am force-stop "$PACKAGE"
sleep 2
adb_ logcat -c
adb_ shell am start -n "$ACTIVITY" --es snapshot_in "$SNAPSHOT_DEV" >/dev/null
sleep 15
shot 6-corrupt
adb_ logcat -d > "$OUT/logcat.txt" 2>/dev/null || true
if ! grep -Eq "snapshot_(bad|truncated|open_failed)|snapshot_in_failed" "$OUT/logcat.txt"; then
    fail "corrupt snapshot did not produce a named snapshot error"
fi

# --- restore the good file so the device is left in a working state --------------
adb_ push "$SNAPSHOT_HOST" "$SNAPSHOT_DEV" >&2
adb_ shell dumpsys meminfo "$PACKAGE" > "$OUT/meminfo-final.txt" 2>/dev/null || true
adb_ shell dumpsys activity exit-info "$PACKAGE" > "$OUT/exit-info.txt" 2>/dev/null || true
echo "qa-snapshot PASS: see $OUT"

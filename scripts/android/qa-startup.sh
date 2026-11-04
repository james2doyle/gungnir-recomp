#!/usr/bin/env bash
# Cold-boot smoke: launches the app N times and fails on any startup crash.
#
# The SDL thread is SDLActivity's Java thread, whose stack it does not size for
# us (~1 MiB). A runtime function with one oversized local frame therefore
# crashes only on the device, and only sometimes — the game-data identity gate
# did exactly that. This gate runs the boot repeatedly so "it started once"
# stops counting as evidence. Logs are kept per run under build/qa/.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
RUNS=${RUNS:-10}
WAIT_SECONDS=${WAIT_SECONDS:-45}
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi
PACKAGE=com.gungnir.recomp
ACTIVITY=$PACKAGE/.GungnirActivity
LOG_DIR=${LOG_DIR:-"$ROOT/build/qa/startup-$(date +%Y%m%d-%H%M%S)"}
# A cold boot proves itself with the first presented guest frame.
BOOT_PATTERN=${BOOT_PATTERN:-"first frame"}

if "$ADB" "${ADB_ARGS[@]}" shell dumpsys window policy 2>/dev/null | grep -q "showing=true"; then
    echo "device_locked: unlock the device before qa-startup.sh" >&2
    exit 1
fi
if [ -z "$("$ADB" "${ADB_ARGS[@]}" shell pidof "$PACKAGE" || true)" ] && \
   ! "$ADB" "${ADB_ARGS[@]}" shell pm path "$PACKAGE" >/dev/null 2>&1; then
    echo "app_missing: install the APK before qa-startup.sh" >&2
    exit 1
fi
mkdir -p "$LOG_DIR"

failures=0
for run in $(seq 1 "$RUNS"); do
    "$ADB" "${ADB_ARGS[@]}" logcat -c
    "$ADB" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE"
    "$ADB" "${ADB_ARGS[@]}" shell am start -n "$ACTIVITY" >/dev/null
    booted=0
    crashed=""
    for _ in $(seq 1 "$WAIT_SECONDS"); do
        LOG=$("$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I 2>/dev/null || true)
        if printf '%s\n' "$LOG" | grep -q "$BOOT_PATTERN"; then
            booted=1
            break
        fi
        crashed=$("$ADB" "${ADB_ARGS[@]}" logcat -d -b crash 2>/dev/null |
                  grep "Fatal signal" | head -1 || true)
        if [ -n "$crashed" ]; then
            break
        fi
        sleep 1
    done
    "$ADB" "${ADB_ARGS[@]}" logcat -d >"$LOG_DIR/run-$run.log" 2>/dev/null || true
    if [ -z "$crashed" ]; then
        crashed=$(grep "Fatal signal" "$LOG_DIR/run-$run.log" | head -1 || true)
    fi
    if [ "$booted" -eq 1 ] && [ -z "$crashed" ]; then
        headroom=$(printf '%s\n' "$LOG" | grep "stack_space thread=main" | head -1 || true)
        echo "boot $run/$RUNS OK ${headroom}"
    else
        failures=$((failures + 1))
        echo "boot $run/$RUNS FAILED booted=$booted crash=${crashed:-none}" >&2
        if [ -n "$crashed" ]; then
            "$ADB" "${ADB_ARGS[@]}" logcat -d -b crash |
                grep -E "Cause:|backtrace:|#[0-9]+ pc" | head -20 >&2 || true
        fi
    fi
    "$ADB" "${ADB_ARGS[@]}" shell am force-stop "$PACKAGE"
done

echo "qa-startup runs=$RUNS failures=$failures logs=$LOG_DIR"
if [ "$failures" -ne 0 ]; then
    exit 1
fi

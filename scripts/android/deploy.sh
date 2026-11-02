#!/usr/bin/env bash
# Deterministic deploy: install the APK, launch the activity, and prove the
# guest is executing before returning. Installs never auto-start the app, so
# every deploy must end with a verified running session, never a bare install.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
APK=${1:-"$ROOT/android/app/build/outputs/apk/debug/app-debug.apk"}
SHOT=${2:-"/tmp/opencode/scene/deploy-$(date +%Y%m%d-%H%M%S).png"}
ADB=${ADB:-adb}
ADB_ARGS=()
if [ -n "${ADB_SERIAL:-}" ]; then
    ADB_ARGS+=(-s "$ADB_SERIAL")
fi

"$ROOT/scripts/android/install.sh" "$APK"
"$ROOT/scripts/android/run.sh"

for _ in $(seq 1 60); do
    if "$ADB" "${ADB_ARGS[@]}" logcat -d -s gungnir:I 2>/dev/null | grep -q "guest_frame"; then
        "$ROOT/scripts/android/screenshot.sh" "$SHOT" >/dev/null
        echo "DEPLOY_OK pid=$("$ADB" "${ADB_ARGS[@]}" shell pidof com.gungnir.recomp | tr -d ' ') shot=$SHOT"
        exit 0
    fi
    sleep 1
done
echo "deploy_failed: no guest_frame after launch" >&2
exit 1

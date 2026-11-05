#!/usr/bin/env bash
# Self-serve PPSSPP oracle capture (m08q05eh): boots a savestate fullscreen,
# captures the desktop after settle, kills the emulator. No input injection.
set -euo pipefail
STATE="${1:?usage: ppsspp-oracle.sh STATE.ppst [OUT.png]}"
OUT="${2:-/tmp/opencode/scene/ppsspp-oracle.png}"
ISO="${PPSSPP_ISO:-/home/james/Git/gungnir-recomp/build/game/imported.iso}"
WAIT="${PPSSPP_SETTLE:-20}"
flatpak run org.ppsspp.PPSSPP --fullscreen --state="$STATE" "$ISO" \
    > /tmp/opencode/ppsspp-oracle.log 2>&1 &
EMU_PID=$!
sleep "$WAIT"
import -window root "$OUT"
kill $EMU_PID 2>/dev/null || true
echo "captured $OUT"

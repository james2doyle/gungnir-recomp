# QA — Android shell (T9)

Date: 2026-10-30
App commit: `9d04446` (T9e); T9f adds docs/scripts only.
APK: debug, `com.gungnir.recomp`, versionName `0.1.0`.
Sign-off: **PASS** for the T9 shell scope.

## Devices

| Device | SoC / GPU | ABI | Android | Role |
|---|---|---|---|---|
| Xiaomi M2102K1AC (Mi 11 Ultra) | Snapdragon / Adreno | arm64-v8a | 14 (API 34) | primary phone |
| Android emulator `sdk_gphone64_x86_64` | SwiftShader | x86_64 | 17 (API 37) | secondary |

The phone was unlocked for the run; a locked keyguard is a named failure
(`device_locked`), not a test pass.

## Build and install

```sh
scripts/android/build.sh -Pgungnir.sdkDir=/opt/android-sdk \
  -Pgungnir.javaHome=/usr/lib/jvm/java-21-openjdk
ADB_SERIAL=30c1f958 scripts/android/install.sh
```

Expected: APK builds from a relocated tree with no `local.properties`;
install succeeds. Actual: pass, `tests/test_android_build.py` 7/7.

## TC-9.1 — clean-checkout APK

`tests/test_android_build.py` copies only the build inputs to a temp tree,
runs `build.sh` with `-P` properties and checks the APK. Actual: pass;
wrapper SHA, SDK/JDK validation and compile-command assertions pass.

## TC-9.2 — first frame

Phone logcat:

```text
I gungnir : first frame 3063x1335
```

Emulator logcat:

```text
I gungnir : first frame 640x320
```

Screenshots were captured locally under `build/qa/android/` (not committed).
Actual: pass. The frame is the synthetic T7 triangle, not game output.

## TC-9.3 — game-data paths and identity gate

Real disc ISO supplied locally (not committed).

- **adb push** to `files/game/game.iso`:
  `game_data_ok ... size=262766592 crc=183F3EDC`.
- **SAF picker** imported the same ISO to `files/game/imported.iso`:
  `game_data_ok origin=saf size=262766592 crc=183F3EDC`.
- Truncated `bad.iso` alone: `game_data_unavailable: identity_size_mismatch`.
- Corrupt candidate beside a valid one: the valid candidate won.

Expected: both paths work; invalid data fails closed. Actual: pass.

## TC-9.4 — lifecycle, audio and orientation

`scripts/android/lifecycle.sh` ran three HOME/resume cycles on both devices.

Phone:

```text
audio_paused / lifecycle_paused
audio_resumed / lifecycle_resumed 3063x1335
```

Emulator:

```text
audio_paused / lifecycle_paused
audio_resumed / lifecycle_resumed 640x320
```

The same process survived all cycles. With `user_rotation 0` (portrait lock)
the app stayed landscape at 3063x1335. Expected: pass.

## Cold boots

Three consecutive force-stop/start cycles on the phone produced `first frame
3063x1335` each time, with `game_data_ok`, `affinity_applied cpu=7`,
`priority_applied nice=-10` and `audio_tone_ready` in each run. Actual: pass.

## Tuning

Phone: `affinity_applied cpu=7`, `priority_applied nice=-10`.
Emulator: `affinity_applied cpu=3`, `priority_applied nice=-10`.
Denial paths are host-tested and log without failing boot.

## Known gaps

- No game rendering; the title screen is T13.
- Synthetic lifecycle tone, not game audio (T12).
- Custom Adreno driver selection is T10.
- Fixed LandscapeLeft (no 180° flip).
- Affinity chooses the highest allowed CPU, not a topology-aware big-core set.
- Debug APK only; release signing is not configured.
- No progress UI for the SAF copy; a large ISO shows the picker’s own spinner.

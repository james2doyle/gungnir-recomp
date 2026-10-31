# QA — native touch overlay and versioned layout (T11b)

Date: 2026-10-31
App commit: `1fe90b3` (the APK was built from that tree).
APK: debug, `com.gungnir.recomp`, versionName `0.1.0`.
Sign-off: **PASS for the T11b on-device cases.** Haptics (T11c), gamepad
hiding (T11d) and the consolidated sprint gate (T11e) are not part of this
report.

## Devices

| Device | SoC / GPU | ABI | Android | Role |
|---|---|---|---|---|
| Xiaomi M2102K1AC (Mi 11 Ultra) | Snapdragon / Adreno | arm64-v8a | 14 (API 34) | primary phone |

The phone was unlocked and on the system Vulkan driver. Screenshots are in
gitignored `build/qa/android/`; no game content is committed.

## 1 — Overlay draw and safe area

Build, install, launch:

```sh
scripts/android/build.sh \
  -Pgungnir.sdkDir=/opt/android-sdk \
  -Pgungnir.javaHome=/usr/lib/jvm/java-21-openjdk
ADB_SERIAL=30c1f958 scripts/android/install.sh
ADB_SERIAL=30c1f958 scripts/android/run.sh
```

Expected: the overlay draws the floating-stick marker, 8-way d-pad, four
face buttons, L/R, SEL/START, with no control under the status bar or the
system-side black band; the Controls and Drivers buttons stay clickable.
Actual: pass (`t11b-default.png`). The Drivers button was moved to the
bottom-right after the first run because it covered R at the top-right.

## 2 — Touch reaches the C++ state

A temporary diagnostic log in the JNI bridge (removed after the run)
reported:

```text
touch_down id=0 x=2551.0 y=1015.0 buttons=00004000
```

`0x4000` is the PSP Cross bit. The Cross ring drew filled while held
(`t11b-cross.png`). The floating stick drew its active ring and thumb at
the touched origin after a drag (`t11b-stick.png`), and the d-pad up arm
drew filled (`t11b-dpad.png`). Releasing each pointer cleared the visual
and the state.

Expected: touch-down reaches the T11a Touch source, pressed visuals follow,
and release clears. Actual: pass.

## 3 — Edit, persist and reload

Steps on the phone:

1. Tap **Controls** to enter edit mode.
2. Drag Cross from its default centre to the left by about 250 px.
3. Tap **Controls** again to leave edit mode.
4. Inspect the app-private layout file.
5. Force-stop, relaunch, inspect the file and the overlay.

The file changed from the default Cross `cx=0.82, cy=0.74` to
`cx=0.7389886379, cy=0.6332540512`, with `radius` unchanged at `0.05`
(`radius` only changes when the rim handle is dragged). After force-stop
and relaunch the same values were present and the Cross ring drew at the
moved position (`t11b-reload.png`).

A long-press on **Controls** restored `cx=0.82, cy=0.74, radius=0.05`.
The reset was written back to the layout file.

Expected: edit drag changes the normalised position, the atomic write
survives restart, reset restores the default. Actual: pass.

## 4 — Corrupt layout fallback

The app-private file was overwritten with `{bad`, then the app was
force-stopped and relaunched:

```text
W GungnirTouch: touch_layout_corrupt_default
I gungnir : first frame 3063x1335
```

The overlay drew the default layout and the app did not crash. A long-press
on **Controls** rewrote a valid version-1 JSON file.

Expected: corrupt content is named and the default is used; the app still
frames. Actual: pass.

## 5 — Three cold boots

Three consecutive force-stop/launch cycles on the phone:

```text
boot1 FRAME_OK  driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
                first frame 3063x1335
boot2 FRAME_OK  driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
                first frame 3063x1335
boot3 FRAME_OK  driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
                first frame 3063x1335
```

Expected: the valid saved layout loads without a `touch_layout_*` warning
and the app presents a frame each time. Actual: pass.

## Gaps

- Haptic ticks are T11c; this report makes no haptic claim.
- Overlay hiding while a gamepad is active is T11d.
- The x86_64 emulator was not used for touch UI QA; the phone is the
  acceptance device for the overlay.
- The app still presents the shell frame (T9); game/synthetic-fixture
  navigation is T13. The JNI mask above proves the input path, not game
  response.

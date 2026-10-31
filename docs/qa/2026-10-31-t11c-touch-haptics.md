# QA — touch-control haptics (T11c)

Date: 2026-10-31
App commit: `df27849` (the APK was built from that tree).
APK: debug, `com.gungnir.recomp`, versionName `0.1.0`.
Sign-off: **PASS.** Physical feel passed on the phone; the system touch-feedback
setting, stick/edit silence, rapid presses and the no-op paths were checked.
Gamepad rumble and battle haptics are out of scope (T13/M5).

## Device

| Device | SoC / GPU | ABI | Android | Role |
|---|---|---|---|---|
| Xiaomi M2102K1AC (Mi 11 Ultra) | Snapdragon / Adreno | arm64-v8a | 14 (API 34) | primary phone |

Build and install:

```sh
scripts/android/build.sh \
  -Pgungnir.sdkDir=/opt/android-sdk \
  -Pgungnir.javaHome=/usr/lib/jvm/java-21-openjdk
ADB_SERIAL=30c1f958 scripts/android/install.sh
ADB_SERIAL=30c1f958 scripts/android/run.sh
```

## 1 — One tick per button/d-pad down

Pressing Cross, the four d-pad directions, L, R, START and SELECT produced
exactly one request each. A run over L, R, START, SELECT and the four d-pad
directions counted eight successes:

```text
COUNT=8
D GungnirTouch: touch_haptic performed=true
```

The vibrator service recorded the app's effect:

```text
com.gungnir.recomp (uid=10557, deviceId=0)
reason: performHapticFeedback: View#performHapticFeedback
played: Prebaked=TEXTURE_TICK(STRONG, with fallback)
duration: 24ms | usage: TOUCH
```

Five rapid Cross taps produced five requests (`performed=true` five times),
so a genuine rapid press is not swallowed.

## 2 — Stick, slide, edit and release are silent

A floating-stick touch, a drag to the right and release produced no
`touch_haptic` log and no vibrator-service entry. Entering edit mode and
dragging/resizing a control also produced none. The per-gesture counts were
stick `0`, d-pad `1`, edit `0`.

Expected: only a new face-button/L/R/START/SELECT/d-pad touch-down ticks.
Actual: pass.

## 3 — System touch-feedback setting

With `haptic_feedback_enabled=0` the app logged:

```text
D GungnirTouch: touch_haptic_disabled
```

and no `com.gungnir.recomp` effect appeared in the vibrator history while
the overlay still showed the pressed state and input kept working. Setting
it back to `1` restored the tick.

Expected: the system setting is respected, and a disabled setting never
blocks input. Actual: pass.

## 4 — Failure and no-op paths

- A throwing sink is contained by the host policy test; later presses still
  work and no input state changes.
- An absent sink is a no-op in the host policy test.
- The x86_64 emulator has a virtual vibrator, so it performed a tick rather
  than proving the no-vibrator path. `performHapticFeedback` returning false
  and the host absent-sink case cover the no-op behavior; no exception or
  stuck pressed state was observed.

## 5 — Physical sign-off

Manual checklist on the phone: one short tick for every face button, L/R,
START/SELECT and d-pad direction; no tick while dragging the analog stick or
resizing in edit mode; silence with system touch feedback disabled while
input still works. Result: **passed.**

## Gaps

- Gamepad rumble and `sceRumble`/battle haptics are T13/M5, not T11c.
- The consolidated T11 gate and three-cold-boot sign-off are T11e.

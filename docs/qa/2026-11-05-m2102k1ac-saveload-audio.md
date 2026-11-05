# Save/load + audio smoke — M2102K1AC — 2026-11-05

Date: 2026-11-05
Build: `f7f8c22` (dirty: `M tests/ge/interpolation_tests.cpp` parked host-test WIP plus `M .dex/tasks.jsonl` bookkeeping; native and android inputs clean)
APK: `android/app/build/outputs/apk/debug/app-debug.apk`
APK SHA-256: `74c41bd133cba3e0cdf4c44b37b3cb3d1540eac4918b089c920ea7a367800b09`

Sign-off: **PASS — memstick round trip and audio smoke on M2102K1AC.**

Part of T14a (QA steps 7–8). Same session as the TC-14.1 run (pid 4149).

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 |
| SoC platform | SM8350 |
| GPU | Adreno (system driver) |
| ABI built | `arm64-v8a` |
| Serial | `30c1f958` |

## Steps

1. In the game's own menu: save, then load the save back.
2. Play with music/SFX; sample `audio_mixer` counters twice.

## Expected and actual

| # | Step | Expected | Actual | Pass |
|---|---|---|---|---|
| 1 | Save through the game's menu | ms0:/PSP/SAVEDATA/ entry written | `files/psp/PSP/SAVEDATA/ULUS10592GUN9THSTORY00/` written 14:57: PARAM.SFO (4912 B) + STORY (61888 B) plus game-supplied ICON0/PIC1 | Pass |
| 2 | Load the save back | Progress restored, no fault | User-driven load-back, reported nominal | Pass |
| 3 | Music/SFX audible | Audible, no distortion reported | User played through menu/session; `peak=32120` (live signal) | Pass |
| 4 | No underrun growth, no xruns | Counters flat | `underrun` 1836200 → 1836200; `overruns=0`, `rejected=0`, `xruns=0`, `muted` flat | Pass |

## Notes

- Pre-existing `ULUS10592GUN9THSYS/` (Nov 2, SYSTEM 72 B) untouched.
- Saves live in app-private internal storage (`run-as` verified); first listing read stale (Nov 2) because it ran before the write — re-check showed today's write. No defect.
- Save contents never pulled into the repo (game content stays on-device).

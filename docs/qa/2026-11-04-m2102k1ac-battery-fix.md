# 2026-11-04 — Static-scene skip + opt-in coverage (battery drain, device)

Date: 2026-11-04. Build: `5fb2b2f` + `-dirty` (uncommitted power-drain fix;
the tree is deliberately dirty pending the user QA gate). APK SHA-256:
`809e2494d2ed3bbd183762b404d5b786b372c8a169d7576b7a59cce6397ec8e1`.
Raw measurements (CPU samples, log lines, battery readings):
`build/qa/battery/measurements.txt` and `build/qa/battery/fix-verification.txt`
(gitignored, reported by hash).

## Device

| Field | Value |
|---|---|
| Model | Xiaomi M2102K1AC |
| Serial | 30c1f958 |
| SoC/GPU | Adreno (arm64-v8a) |
| ABI | arm64-v8a |
| Framebuffer | 3063x1335 |
| Display | 120 Hz |
| Charger | USB (AC=false), weak, 227-675 mA observed |
| Scene | battle snapshot, 2793 draws, interpolation ON (persisted pref) |

## Root cause (from the investigation)

The app's sustained compute/render load exceeded the weak USB charger, so the
battery net-discharged while Android reported `status=2` (charging). Two
avoidable loads: interpolation generating in-between presents for a **static**
scene (`interpolation_pairs motion=0.0`, ~2793 candidate pairs/pass, ~39
presents/s on a 120 Hz display), and `measure_widescreen_coverage` running on
**every present** with no way to turn it off. Full attribution in
`docs/BRINGUP.md` (2026-11-04 battery section).

## Steps

1. Write red tests first (`present_schedule_tests`, `scene_motion_tests`,
   `test_power_diagnostics.py`); observe them fail for the named reason.
2. Implement `scene_motion`/`scene_is_static`, a `static_scene` argument to
   `PresentSchedule::on_guest_frame`, and opt-in coverage gating.
3. Mutation checks: break each semantic, confirm the suite dies for that
   reason; restore.
4. Host build + `ctest` (2/2) + `repository_check` (PASS).
5. Build the debug APK offline for `arm64-v8a`; install `-r`; force-stop.
6. Boot with the battle snapshot via `--es snapshot_in` (no coverage extra).
7. Measure CPU over 8 s; read `interpolation active=... static_frames=...`,
   `present_time`, `timeline presents`, `presents published`.
8. Trigger a camera pan (stick swipe) and confirm interpolation resumes.
9. Relaunch with `--el coverage 1` and confirm the opt-in path; relaunch with
   no extra to confirm coverage stays off by default.
10. Leave the device in the normal QA state (interpolation ON, coverage OFF).

## Expected / actual

| Step | Expected | Actual | Pass |
|---|---|---|---|
| 1 | Red tests fail for the named reason | compile errors (`scene_motion` missing, 3-arg `on_guest_frame` missing) + 6 Python failures | PASS |
| 3 | Mutation checks kill the suites | "a moved scene is not static", "a static frame is presented immediately", coverage guard | PASS |
| 4 | Host gates green | `ctest` 2/2, `repository_check` 505 files clean | PASS |
| 5 | APK builds | BUILD SUCCESSFUL, main.cpp compiles with NDK | PASS |
| 7 | CPU drops; no idle presents | 86% -> 61% one core; `interpolated=0`, `static_frames=292/294`, presents 1:1 with guest (20 Hz), `present_time avg 10920 -> 8041 us` | PASS |
| 8 | Motion still interpolates | pan -> `interpolated=35`, `motion=20.3`, `per_frame=0..3` | PASS |
| 9 | Coverage off by default, on with `--el coverage 1` | no coverage lines by default; `coverage opt-in enabled` + periodic line with the extra | PASS |
| 10 | Device restored | `frame_interpolation active=1`, no coverage line, `static_frames=94 interpolated=0` | PASS |

## Named gaps / honesty

- A 12 s window held battery level 17 -> 17 at 58% CPU, but level is a coarse
  integer percent and the charger is weak (227-675 mA). **Net charge recovery
  is not claimed**; a longer charge-rate test on a proper high-current charger
  is needed. The fix demonstrably cuts the app's continuous CPU load by ~29%.
- `--es coverage 1` (string extra) does NOT enable coverage: `intent_long`
  reads a long via `getLongExtra`. Use `--el`.
- A lone small moving element in an otherwise still scene is now held at the
  guest frame rate (the static threshold `kStaticScenePixels = 1.0` guest px is
  mean motion). This is an intentional trade for the experimental interpolation
  feature; watch for it in scene-sweep QA.
- Whether the drain was a regression vs a prior build is still unconfirmed.

## Phase 2 — PPSSPP-derived pacing improvements (same day)

Three further changes from the PPSSPP reference survey, each red-first with a
mutation check. Raw numbers: `build/qa/battery/phase2-verification.txt`
(gitignored). Child tasks `eacou9zl`, `x0d44mv7`, `7rb8oi1d`.

| Change | Expected | Actual | Pass |
|---|---|---|---|
| #1 cap 60 Hz | `capped_hz=60.0` on a 120 Hz display; pan `per_frame` reduced | `display_refresh_hz=120.0 capped_hz=60.0`; pan `per_frame=0..2` (was `0..3`), `interpolated=32 motion=20.5` | PASS |
| #3 deadline wait | wait to real deadline, 16 ms ceiling; touch still responsive | tap → `frame_interpolation active=0` within 101 ms (adb polling overhead included); toggle responded, setting restored ON | PASS |
| #4 compositor retention | last image survives no presents | ~7 s and ~53 s with ZERO presents: game image fully visible, guest ran to frame 1140, no surface errors | PASS |
| #4 exact hold | `presents_skipped` only for byte-identical frames | dump of 8 consecutive frames: 0–4 draws differ, 0.003–0.027 px motion → `presents_skipped=0` on this scene (as predicted) | PASS (falsified premise, recorded) |
| combined | lower CPU | 58 % one core (original 86 %, phase-1 61 %), `present_time avg_us` ~7027, `static_frames` ~99 %, pan 59 % | PASS |

### Named gaps (phase 2)

- #4's exact-equality premise is **falsified on animated scenes**: consecutive
  "static" frames differ (sub-pixel jitter). The safe exact hold rarely triggers
  here; a threshold-based hold would save more CPU but risks freezing subtly
  animated HUD/cursor elements — **not implemented**, needs dedicated visual
  scene-sweep QA + a user decision.
- #3's 16 ms responsiveness ceiling raises worst-case touch latency from 8 ms;
  measured prompt, but a longer structured touch test is still warranted.
- Net charge recovery still not claimed (weak 191 mA charger, coarse level).

## Sign-off

Device deployed for hands-on testing (`app-debug.apk` sha-256
`74c41bd133cba3e0cdf4c44b37b3cb3d1540eac4918b089c920ea7a367800b09`): fresh
install and cold boot with the battle snapshot, `frame_interpolation active=1`,
`display_refresh_hz=120.0 capped_hz=60.0`, `static_frames` ~99 %,
`present_time avg_us` ~7052, CPU 57 % of one core, no `render_failed` /
`session_error` lines.

**User sign-off (2026-11-04):** "does seem better" — accepted as the user
sign-off for the battery-drain fix and the phase-2 pacing work. The three-cold-boot
requirement of `.dex/plan.md` §8.3 is waived by the user for this landing; the
structured device matrix, build SHA, steps and expected/actual above stand as
the recorded QA evidence.

The commit intentionally EXCLUDES `tests/ge/interpolation_tests.cpp`, which
holds a known failing reproduction test for the separate wall-folding artifact
(open work). No red test case lands in this commit. Nothing references that
test file outside its own CMake target.

Automated gates (`ctest` 2/2, `repository_check`, all ge/Python suites) are
evidence, not a substitute for the device sign-off above.

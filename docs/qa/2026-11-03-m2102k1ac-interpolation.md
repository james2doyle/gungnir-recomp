# QA — frame interpolation cell (T14a matrix)

Date: 2026-11-03
Build: committed as `6eb601a` (stack-frame fix and gates) and `8137af1`
(schedule ticks at the panel rate); the APK was built from those sources and
rebuilt afterwards to confirm the recorded hash, so no `-dirty` state is
involved. Corpus: `build/generated` (AOT), `arm64-v8a` only.
APK: `android/app/build/outputs/apk/debug/app-debug.apk` (debug, `arm64-v8a`)
APK SHA-256: `95dade69c079fd31892f4a3dc757e3152c8dba79a724b84e55932fce5e592e26`

Sign-off: **PASS — the option is off by default, toggles from the settings row
without a restart, persists as host state, and doubles the presented frame
rate in the battle scene (19.8 → 41.1 presents/s) while the guest keeps its
20 fps logic rate, real frames stay correct with no smearing, audio shows no
`overruns`/`xruns` growth, and the ten-launch cold-boot gate is clean.** One
scope limitation is recorded as a named gap: the 4:3 versus 16:9 comparison
belongs to the T16 presentation work, which is not in the tree.

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 (SDK 34) |
| SoC platform | SM8350 / lahaina |
| GPU | Adreno (system driver, 329cf4c2a7) |
| Display | 3200x1440, panel 120 Hz, app framebuffer 3063x1335 |
| ABI built | `arm64-v8a` |
| Serial | `30c1f958` |

## Steps

1. Build the APK with `scripts/android/build.sh --offline
   -Pgungnir.abis=arm64-v8a -Pgungnir.gameCorpus=build/generated`, install,
   push the battle snapshot to
   `files/snapshots/snapshot.gsnap`, launch with
   `am start -n com.gungnir.recomp/.GungnirActivity --es snapshot_in <path>`.
2. Run `scripts/android/qa-startup.sh` (ten cold boots) on this artifact: no
   launch may leave a `Fatal signal`, and each records
   `stack_space thread=main`.
3. Arm A (off): launch at the battle snapshot with no intent, let it run 25 s,
   read `interpolation active=`, `presents`, `present_time`,
   `audio_mixer`, `cadence` every 5 s, and capture a screenshot.
4. Arm B (on): same snapshot with `--el frame_interpolation 1`, same duration
   and reads.
5. Compare the two screenshots against each other and against the T17a.1
   baseline: the scene, HUD and text must be identical apart from animation
   phase, with no smearing or ghosting on the interpolated frame.
6. Open the settings gear at the battle scene: the row must read
   `Frame interpolation: Off`, and tapping it must show the toast, read `On`
   and produce `interpolation active=1` in the log within one log tick.
7. Force-stop and relaunch with no intent: the row must still read `On`
   (persistence), and `first frame` must still be the boot proof. Toggle back
   to `Off` afterwards so the device returns to the default.
8. Check logcat for crashes and ANRs across the whole cell.

## Expected and actual

| Step | Expected | Actual |
|---|---|---|
| Cold-boot gate | 10 launches, no crash | Pass: `runs=10 failures=0`; every launch `stack_space thread=main limit_kb=1043 used_kb=9 headroom_kb=1033` |
| Arm A (off) | Guest rate, `interpolated=0` | Pass: `flips=499` in 25.0 s (19.9/s), `presented=496` (19.8/s), `interpolated=0 held=0 skipped_ticks=0` |
| Arm B (on) | In-betweens presented, real frames still shown | Pass: 1008 in-betweens + 6 held over 24.7 s (41.1 presents/s), `late=191` |
| Guest logic rate | Unchanged by the option | Pass: `flips=499` in both arms; `draws3d=1303887 draws2d=110279 vertices=13057210` identical in both arms |
| Visual | Same scene, no smearing | Pass: both screenshots show the same battle at the same camera, HUD and text unchanged; the interpolated frame differs only in sprite/cursor phase |
| Option row | Off, toggles, toasts | Pass: `Frame interpolation: Off` → tap → toast `Frame interpolation on`, row `On`, log `interpolation active=1` |
| Persistence | Survives a restart | Pass: relaunch with no intent still reads `On`; toggled back to `Off` |
| Audio | No overrun/xrun growth | Pass: both arms `overruns=0 rejected=0 xruns=0`, `underrun` frozen at 3175 (pre-existing), `peak=0` in both |
| Crashes | None | Pass: no `Fatal signal`, no ANR, no `session_stop` |

## Counters

Battle scene, same snapshot, same artifact, 25 s per arm (log ticks every 5 s):

| Measure | Arm A (off) | Arm B (on) |
|---|---|---|
| Guest flips | 499 (19.9/s) | 499 (20.2/s) |
| Presents recorded | 496 (19.8/s) | 1015 (41.1/s) |
| In-betweens / held | 0 / 0 | 1008 / 6 |
| `skipped_ticks` | 0 | 1417 |
| `present_time` avg | 9,273 µs | 13,888 µs |
| `present_due` min (drift) | −1,420 µs | −7,987 µs |
| `late` presents | 2 | 191 |
| `dropped` | 3 | 3 |
| Draws per flip | 2613 3D + 221 2D | 2613 3D + 221 2D |
| Vertices per flip | 26,167 | 26,167 |
| `overruns` / `xruns` | 0 / 0 | 0 / 0 |

Title screen (toggle check, cheaper draws): `interpolated=2047`, `held=22`,
`skipped_ticks=77` — the shortfall counter tracks the scene's present cost, as
designed.

## Screenshot SHA-256

Images stay under gitignored `build/qa/`. Logs for both arms are
`build/qa/t17a8_off.log` and `build/qa/t17a8_on.log` (also gitignored).

- Arm A (off), battle: `4aa685477952516ec90dd4890dc3933754df3e2e364e7d745bdb02613d8f399e`
- Arm B (on), battle: `e4654cd5e21182f5d630040c9ff1567123d190e09599cb744dd2c604205e7fd1`
- Settings row off: `0b2b84240dc0fb85ca0730a6b73c2fd1b2ff325dd611362e265c7956cda041f4`
- Settings row on, with toast: `a50c123d1f572be9908edf1bfcf5759bf9fbd95e8042cbf4ced068f23459447f`
- After restart, still on: `c069517856f86856b118e41f685319daabfe120f7c4a28bc0f00d3b08962e29b`
- Toggled back to off: `e57ebb38cda88237bab49685c12e453e95e773a5dd7dc573ca41303b5f2b9a43`

## Findings

- **The startup stack overflow is fixed and gated (pre-existing, blocked this
  cell).** The game-data identity gate held a 1 MiB scratch buffer on the SDL
  thread, whose stack is 1043 KiB with 9 KiB already in use: any small
  addition tipped it into a SIGSEGV at boot. The buffer is heap-allocated, the
  runtime libraries compile with `-Werror=frame-larger-than=16384`, the gate is
  tested on a 512 KiB thread, and `scripts/android/qa-startup.sh` repeats the
  cold boot; ten consecutive launches were clean on this artifact. Detail in
  `docs/BRINGUP.md`.
- **The schedule now ticks at the panel rate.** The refresh interval was
  measured and logged but never installed, so in-betweens were offered at
  60 Hz on a 120 Hz panel. With the panel rate installed the presented rate
  doubles (19.8 → 41.1/s) and `skipped_ticks` states the refreshes the
  presenter cannot hold.
- **Present cost rises with the faster cadence (HYPOTHESIS).** The average
  present grows from 9.3 ms to 13.9 ms although the draw list is identical
  (2613 3D + 221 2D draws, 26,167 vertices in both arms), and `late` grows
  from 2 to 191. The likely cause is the swapchain acquisition waiting for an
  image to be released at the panel rate rather than extra GPU work; the
  ceiling is the presenter, not the renderer, which matches the T17a.1
  measurement that the renderer is idle. Not investigated further in this
  cell; the counters name it.
- **Named gap: 4:3 versus 16:9.** The cell asks for produced and presented fps
  at both aspects. Presentation layout belongs to the T16 widescreen work
  (fill-screen presentation, then projection widening), which is not in the
  tree, so this build only has the fitted display aspect and the game is
  letterboxed. The comparison is **not** recorded here and the cell is signed
  for the aspect the app actually presents; the T16 work delivers the aspect
  modes and their frame-rate rows.
- **Audio is unchanged by the option.** Both arms report identical
  `audio_mixer` counters except for the amount pulled; `overruns` and `xruns`
  stay at zero, so the extra presents do not starve the mixer.

## Notes

- The battle was reached from the host snapshot at
  `files/snapshots/snapshot.gsnap` (session start then `session_resume
  pc=0x08990000`); the guest itself loaded nothing.
- Thermal/battery: 29.0 °C, 93 % at the end of the cell; no throttling
  observed, no ANR, no watchdog stall in the bounded runs.
- No game frames, dump hashes or generated content are stored with this
  record; screenshots and logs stay under the gitignored `build/qa/` tree and
  only their SHA-256 values are published here.

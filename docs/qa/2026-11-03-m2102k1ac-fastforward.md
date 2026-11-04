# QA — fast-forward cell (T14a matrix)

Date: 2026-11-03
Build: working tree on top of `fef8056` (fast-forward change uncommitted,
`-dirty`); the temporary untextured debug probe that was present in
`runtime/ge/vulkan_backend.cpp` was reverted before this APK was built, so
the artifact carries none of it
APK: `android/app/build/outputs/apk/debug/app-debug.apk` (debug, `arm64-v8a`,
AOT corpus `build/generated`)
APK SHA-256: `520958d0f9bb965c705a7970f5de781791ae8c86fb4c5e9ef1ce2a256e539d4f`

Sign-off: **PARTIAL PASS — fast-forward runs the guest at 3x in a battle
scene, drops audio without overrun/xrun growth, keeps its latch across a host
snapshot save and load, and reports state on the touch ring plus a toast.**
The gamepad R2-hold and R3-toggle paths were **not** exercised: no physical
controller was attached to this device during the session. That half of the
cell stays a named gap (host contracts cover it; see "Findings").

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 (SDK 34) |
| SoC platform | SM8350 / lahaina |
| GPU | Adreno (system driver) |
| ABI built | `arm64-v8a` |
| Serial | `30c1f958` |

## Steps

1. Build the corpus (`analyze` + `codegen` → `build/generated`), build the
   APK with `scripts/android/build.sh -Pgungnir.gameCorpus=build/generated
   -Pgungnir.abis=arm64-v8a --offline`, install and launch with
   `scripts/android/deploy.sh` (frame proof `FRAME_OK`).
2. Cold boot: confirm `game_data_ok`, `session_start`, `first frame` and
   flowing `guest_frame` lines.
3. Confirm the shipped touch layout is a version 1 file on the device, load
   it, tap the FF ring (left of SELECT), then background once so the view
   saves the layout. Re-read `files/touch_layout.json` with `run-as`.
4. Toggle fast-forward by tapping the FF ring: expect the latch log line,
   the ring fill, the `Fast-forward 3x` toast, and the mixer mute counters.
   Toggle again and expect `active=0`, the ring outline, and the
   `Fast-forward off` toast.
5. Measure 3x with two bounded runs on the same artifact, same scene:
   `am start ... --el wall_seconds 55` at 1x, then the same with
   `--el fast_forward 1`. Compare `vblanks` (emulated clock), `flips` (guest
   progress) and `dispatches` per wall second.
6. Save a host snapshot while the latch is on, verify `snapshot_saved` and
   `session_resume`, then load it and verify `snapshot_loaded` reports the
   same resume PC and frames continue. Tap the ring once afterwards.
7. In battle: latch fast-forward by touch, play 77 s, read the mixer line
   every 5 s, then unlatch and confirm audio resumes and frames keep flowing.
8. Check logcat for crashes, ANRs and `session_stop` during the cell.

## Expected and actual

| Step | Expected | Actual |
|---|---|---|
| Cold boot | Session starts, frames flow | Pass: `first frame 3063x1335`, `guest_frame=60` |
| Layout load | Version 1 file loads without reset, geometry kept | Pass: no `touch_layout_older_default`; `dpad` radius `0.13709422945976257` preserved |
| Layout save | Migrates to version 2 with the FF control | Pass: `version 2`, 11 controls, `fast_forward` at `(0.43, 0.08)` r `0.038`, previous geometry intact |
| FF on (touch) | `active=1 speed=3.00`, ring lit, toast | Pass: log line at 17:34:24.451; `Fast-forward 3x` toast; ring drawn filled |
| FF off (touch) | `active=0 speed=1.00`, ring outline, toast | Pass: 17:35:46.185; `Fast-forward off` toast; ring outline |
| Audio while FF | Guest buffers dropped, no overrun/xrun | Pass: `submitted` frozen, `muted` +24,225 over 46 s, `overruns=0 rejected=0 xruns=0`, `underrun` grows (expected silence) |
| Audio after FF | Mixing resumes, no overrun | Pass: `submitted` 12,733 → 13,595 → 14,456 across 10 s; `muted` frozen; `overruns=0 xruns=0` |
| 3x pacing | ~3x guest time per wall second | Pass: see counters (3.01x vblanks, 2.99x flips) |
| Battle FF | Same 3x, audio muted, no wedge | Pass: 76.7 s latched in battle; counters clean; frames flowing afterwards |
| Save while latched | Saves, session resumes, latch kept | Pass: `snapshot_saved ... bytes=35904716`, `session_resume pc=0x08985C44`; latch still on |
| Load while latched | Restores the same point, latch kept | Pass: `snapshot_loaded ... resume_pc=0x08985C44`, frames continue; one tap then reports `active=0` |
| Crashes | None | Pass: no `FATAL`, no `ANR`, no `session_stop` in the cell |

## Counters

Bounded runs, 55 s wall each, same artifact and same title scene:

| Run | dispatches | flips | vblanks | vblanks/s | flips/s |
|---|---|---|---|---|---|
| 1x (`wall_seconds 55`) | 260,000 | 1,119 | 3,313 | 60.2 | 20.3 |
| 3x (`wall_seconds 55` + `fast_forward 1`) | 740,000 | 3,342 | 9,976 | 181.4 | 60.8 |
| ratio | 2.85x | 2.99x | 3.01x | 3.01x | 2.99x |

The emulated clock (`vblanks`) is the honest pacing measure: 3.01x. Guest
progress (`flips`) tracks it at 2.99x; dispatch count grows slower (2.85x)
because the guest idles more per unit of wall time.

Battle fast-forward (touch latch, 17:40:20.720 to 17:41:37.432, 76.7 s):

- mixer samples every 5 s: `submitted=12624` frozen for the whole window,
  `muted` 14,258 → 40,740, `overruns=0 rejected=0 xruns=0`, `peak=28904`
  frozen (music was audible before the latch).
- after unlatching: `submitted` 12,733 → 13,595 → 14,456, `muted` frozen at
  40,740, `underrun` frozen, `peak` rose to 32,768.
- frames after the cell: `guest_frame=5460 draws=2663 vertices=25117`,
  `guest_frame=5520 draws=2651 vertices=24615` — a full battle draw list,
  still flowing.

## Screenshot SHA-256

Images stay under gitignored `build/qa/2026-11-03-m2102k1ac-ff/`.

- Title, real textures: `8e22dfb34e5f7436a2095cd52d2eeb40e13231b06b35b5e7965d726f767bab1a`
- FF on, toast and lit ring: `12c6f529ea3054b2b07b0d35bc9523bb4562b4682be7468625d2f42c824c9bf4`
- FF off, toast and outline ring: `e2733db8f4c6d52b398e1b3d84f980fc2843d83a61ae99ddde3df941ff7a12eb`

## Findings

- **Gamepad half not run (named gap).** No controller was attached, so R2
  (right-trigger axis over half, momentary) and R3 (right-stick click, a
  toggle, only while the profile leaves it unbound) were exercised only by
  `tests.test_sdl_gamepad`
  with a virtual pad, not on this device. The cell stays PARTIAL until a pad
  session covers them.
- **Backgrounding ends the session (pre-existing, tracked).** Pressing HOME
  logs `lifecycle_paused`, the worker prints its `session_unbound` summary,
  and returning logs `lifecycle_resumed` with no `session_start` and no
  further `guest_frame`: the app shows the last frame for good. Filed as
  `eqomlp5a`. This also puts the rendering row of
  `docs/qa/2026-11-03-m2102k1ac-resume.md` under re-check, because a
  `grep -c guest_frame` over the log buffer counts pre-pause lines. The
  fast-forward latch itself did survive the round trip (a single tap after
  resume cleared it), which is what this cell needed.
- **Post-session correction (R3 semantics).** After this session R3 became a
  latch toggle (R2 stays the momentary hold, which matches this report's
  wording). No row above is affected: the gamepad half was not run here. The
  host contract `tests.test_sdl_gamepad` covers the merge (an R2 hold plus an
  R3 latch keeps running after the trigger is released) and a second R3
  press clearing it.
- **No snapshot format change.** The speed is host policy and never enters
  the save or its identity; a state saved at 3x restores at whatever the
  user has selected afterwards.

## Notes

- The PPSSPP state (`ULUS10592_1.00_0.ppst`) was not used: the container is
  never parsed in this repository (plan TC-12.6). The battle was reached with
  the game's own `LOAD GAME` from the memstick saves already under
  `files/psp/PSP/SAVEDATA/`.
- The APK first built for this cell carried a local untextured probe; it was
  reverted and the APK rebuilt before any of the steps above, so every result
  here is from the probe-free artifact whose hash is recorded above.
- No game frames, dump hashes or generated content are stored with this
  record.

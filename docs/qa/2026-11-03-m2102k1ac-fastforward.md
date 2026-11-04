# QA — fast-forward cell (T14a matrix)

Date: 2026-11-03
Build: committed as `b0978e1`; the touch, pacing, audio and save/load rows
were observed on the tree as it stood before that commit (`-dirty`), the
gamepad rows on the committed tree. The temporary untextured debug probe that
was present in `runtime/ge/vulkan_backend.cpp` for part of the session was
reverted before the first reported APK was built, so no reported row ran
against it.
APK: `android/app/build/outputs/apk/debug/app-debug.apk` (debug, `arm64-v8a`,
AOT corpus `build/generated`)
APK SHA-256: `520958d0f9bb965c705a7970f5de781791ae8c86fb4c5e9ef1ce2a256e539d4f`
(touch/pacing/audio/save-load rows)
APK SHA-256: `668b8fd15e995be0104000cad9c5056b3f20f690f47965665ab419b88f6bc3c8`
(gamepad rows, `b0978e1`)

Sign-off: **PASS — fast-forward runs the guest at 3x in a battle scene,
drops audio without overrun/xrun growth, keeps its latch across a host
snapshot save and load, reports state on the touch ring plus a toast, and is
driven from the pad (R3 latches and clears, R2 holds).**

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
8. With a controller paired, press R3 once, wait, press it again; then hold
   R2 and release it. Check `fast_forward active=` transitions and the pad's
   mapping (R3 must be unbound in the active profile, or it stays a guest
   button).
9. Check logcat for crashes, ANRs and `session_stop` during the cell.

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
| R3 press | Latches, then clears on the next press | Pass: `active=1` at 17:47:42, `active=0` at 17:48:23 (41 s apart, so a latch, not a hold); pad present (`Classes: KEYBOARD|GAMEPAD|JOYSTICK|EXTERNAL`, `RTRIGGER` axis) |
| R2 hold | 3x only while the trigger is down | Pass (owner-verified on the same pad); the hold and the R3 latch share one control, so the log shows the same transitions; `tests.test_sdl_gamepad` covers the merge |
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

- **Gamepad half (now covered).** R2 is the momentary hold (right-trigger
  axis over half); R3 is a latch toggle (right-stick click, only while the
  profile leaves it unbound). Both were exercised on the pad after the R3
  semantics changed; the log only shows `fast_forward active=` transitions,
  so the 41 s latch above is the R3 evidence and the R2 hold is
  owner-verified plus `tests.test_sdl_gamepad` for the merge and for an R3
  that a profile has bound staying a guest button.
- **Backgrounding ends the session (pre-existing, FIXED later that day).**
  Pressing HOME logged `lifecycle_paused`, the worker printed its
  `session_unbound` summary, and returning logged `lifecycle_resumed` with no
  `session_start` and no further `guest_frame`: the app showed the last frame
  for good. That also put the rendering row of
  `docs/qa/2026-11-03-m2102k1ac-resume.md` under re-check, because a
  `grep -c guest_frame` over the log buffer counts pre-pause lines. The stop now
  resumes in place on foreground; see
  `docs/qa/2026-11-03-m2102k1ac-background-resume.md`. The
  fast-forward latch itself did survive the round trip (a single tap after
  resume cleared it), which is what this cell needed.
- **R3 semantics (late change, then re-checked).** R3 started as a hold in
  the first cut of this work and became a latch toggle, with R2 keeping the
  hold. The pad rows above were run on that later artifact, and the host
  contract `tests.test_sdl_gamepad` covers the merge (an R2 hold plus an R3
  latch keeps running after the trigger is released) and a second R3 press
  clearing it.
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

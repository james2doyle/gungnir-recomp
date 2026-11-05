# Save/load + audio smoke — M2102K1AC — 2026-11-05

Part of T14a (QA steps 7–8). Same session as the TC-14.1 run (pid 4149,
APK 74c41bd1…, commit f7f8c22 dirty as recorded in the TC-14.1 report).

## Device

M2102K1AC, Android 14, SM8350, serial 30c1f958, arm64-v8a.

## Save/load (step 7)

| # | Step | Expected | Actual | Pass |
|---|---|---|---|---|
| 1 | Save through the game's own menu | ms0:/PSP/SAVEDATA/ entry written | `files/psp/PSP/SAVEDATA/ULUS10592GUN9THSTORY00/` written 14:57: PARAM.SFO (4912 B), STORY (61888 B) plus game-supplied ICON0/PIC1 | Pass |
| 2 | Load the save back | Progress restored, no fault | User-driven load-back, reported nominal | Pass |

- Pre-existing `ULUS10592GUN9THSYS/` (Nov 2, SYSTEM 72 B) untouched.
- Saves live in app-private internal storage (`run-as` verified); never
  pulled into the repo (game content stays on-device).

## Audio smoke (step 8)

| # | Step | Expected | Actual | Pass |
|---|---|---|---|---|
| 1 | Music/SFX audible during play | Audible, no distortion reported | User played through menu/session, no complaint; `peak=32120` (live signal) | Pass |
| 2 | No underrun growth, no xruns | Counters flat | `underrun` 1836200 → 1836200 across samples; `overruns=0`, `rejected=0`, `xruns=0`, `muted` flat | Pass |

## Findings

- None. Save timestamps first read stale (Nov 2) because the listing ran
  before the write; re-check showed today's write. No defect.

## Sign-off

Save/load round trip + audio smoke on M2102K1AC: SIGNED.

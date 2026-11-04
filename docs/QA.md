# QA — manual device campaign (T14a)

Physical-device QA is mandatory; "CI is green" is not device evidence and
one successful boot is not evidence. The emulator never closes a cell.
Ask the user for GUI interaction when needed; never `xdotool`. No game
frames, dump hashes or generated content enter the repository: screenshots
stay under gitignored `build/qa/<session>/` and only their SHA-256 is
recorded. PPSSPP is an oracle only.

## Standard script

1. Install the release-candidate APK (`scripts/android/deploy.sh` installs
   and launches with a frame proof; bare `install.sh` always needs `run.sh`
   after it).
2. Cold boot: force-stop the app, clear it from recents, launch from the
   launcher. Record first-frame time and any stall.
3. Title screen: confirm art, menu and copyright line upright at the fitted
   aspect; text legible at 1x.
4. Menu input: drive every entry by touch, then by gamepad (paired pad and
   built-in handheld where available). Overlay hides on pad input.
5. Suspend/resume: background the app (home), foreground it; rendering must
   recover without a restart. Repeat twice.
6. Rotation: rotate the device; the app ignores it (no relayout, no crash).
7. Save/load: memstick save then load through the game's own path; confirm
   the round trip.
8. Audio smoke: music/SFX audible, no `underrun` growth, no xruns.
9. Fast-forward: hold R2, press R3 (a toggle), and tap the FF ring next to
   SELECT (also a toggle).
   Progress must run about 3x per wall second, the ring lights and the
   `Fast-forward 3x` toast shows, audio drops without `overruns`/`xruns`
   growth (mixer `underrun` and `muted` grow, which is the expected mute
   shape), and a latched state survives background/foreground. A held R2 must
   not survive the pause.
10. Soak: 30 minutes of attended play on at least one physical device;
   record crash/thermal/audio counters, not frames.
11. Findings: fix red-first where host-testable (reproducing test before
    the fix); file device-only defects as tracked dex tasks and link them
    from the session report.

## Gungnir-specific script (title → menu → first gameplay)

Title → Cross (touch, then pad) → difficulty selection → d-pad down →
Cross to confirm → first gameplay transition. New-game initialisation
faults are T13i territory: record the fault address and registers, do not
re-diagnose here.

## Matrix

Three consecutive cold boots per device. Every SIGNED cell points at a
dated `docs/qa/` report carrying all sign-off fields; a report that cannot
supply a field says `unknown`.

| Cell | Report | Status |
|---|---|---|
| emulator smoke (SwiftShader x86_64) | 2026-11-03-emulator-smoke.md | SIGNED |
| physical Adreno cold boot x3 | 2026-11-03-m2102k1ac-coldboot.md | SIGNED |
| physical Adreno touch menu | 2026-11-03-m2102k1ac-menu.md | SIGNED |
| physical Adreno gamepad menu | 2026-11-03-m2102k1ac-menu.md | SIGNED |
| physical Adreno suspend/resume | 2026-11-03-m2102k1ac-resume.md | SIGNED |
| physical Adreno fast-forward | 2026-11-03-m2102k1ac-fastforward.md | SIGNED |
| physical Adreno 30-minute soak | TBD | PENDING |
| physical Adreno save/load | TBD | PENDING |
| physical Adreno audio smoke | TBD | PENDING |
| non-Adreno/handheld (optional, if available) | TBD | PENDING |

Note 2026-11-03 (fast-forward cell): signed for touch, pacing, audio,
save/load and pad control (R3 latches, R2 holds). The same session found that
backgrounding ends the guest session (`eqomlp5a`), so the "rendering" row of
the suspend/resume report above needs re-running before that cell is trusted
again.

## Evidence and sign-off template

Each session report is `docs/qa/<date>-<device>.md` with: Date, Build
(commit SHA plus `-dirty` when the tree is uncommitted — never validate a
dirty tree without saying so), APK SHA-256, a Device table (Model,
Android, SoC/GPU, driver + version, ABI, Serial), numbered Steps, an
Expected and actual table with Pass/Fail per step, counters (draws,
`audio_mixer` line, crash/thermal notes), screenshot SHA-256 values, and a
`Sign-off:` line naming the scope that passed. A finding without a
regression test or a tracked open defect fails the cell.

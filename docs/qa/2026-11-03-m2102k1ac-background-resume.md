# QA — background/foreground guest resume (T14a matrix)

Date: 2026-11-03
Build: `7947742` (clean tree)
APK: `android/app/build/outputs/apk/debug/app-debug.apk`
APK SHA-256: `405cf60df8322e6d151e68b4132358a356072ea832d6f7cdc309f7e032ff2f05`

Sign-off: **PASS — a background/foreground round trip keeps the guest, the
progress and the audio.** The OS took the surface away for 5.7 s at a battle,
and the session resumed in place from the pc it stopped at: same PID, no
`session_start`, one `session_resumed reason=foreground` 32 ms after the
surface returned, `guest_frame` lines 1.3 s later at the same 2834-draw battle,
and `overruns=0`/`xruns=0` throughout.

Supersedes the "rendering" row of `docs/qa/2026-11-03-m2102k1ac-resume.md`,
whose evidence counted pre-pause `guest_frame` lines while the session was in
fact stopped and never resumed.

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 |
| SoC platform | lahaina |
| GPU | Adreno (system driver) |
| ABI built | `arm64-v8a` |
| Serial | `30c1f958` |

## Steps

1. Install the APK from a clean tree at `7947742`.
2. Force-stop the app and start it with the battle snapshot:
   `am start -n com.gungnir.recomp/.GungnirActivity --es snapshot_in
   /storage/emulated/0/Android/data/com.gungnir.recomp/files/snapshots/snapshot.gsnap`.
   The snapshot was saved by the owner through the menu at the battle start
   (37,978,215 bytes) so the scene is reproducible.
3. Wait for `session_resume pc=0x08990000` and two `guest_frame` lines
   (battle scene, `draws=2834`). Record the PID.
4. `input keyevent KEYCODE_HOME`; wait 6 s.
5. `am start -n com.gungnir.recomp/.GungnirActivity`; wait 8 s.
6. Read logcat (`-s gungnir:I`) and the PID again; capture a screenshot before
   and after.
7. Run `scripts/android/lifecycle.sh` (three background/foreground cycles from
   a cold start) as the scripted gate.

## Expected and actual

| Step | Expected | Actual |
|---|---|---|
| Background | Guest stops between dispatches, audio pauses and mutes | Pass: `audio_paused`, `audio_muted reason=background`, `lifecycle_paused` at 19:48:20.621; `session_stop cause=host_stop dispatches=50000 ... stop_pc=0x08800004` at 19:48:20.747 |
| Foreground | Surface returns, then the session resumes in place | Pass: `lifecycle_resumed 3063x1335` 19:48:26.314, `session_resumed reason=foreground` 19:48:26.331 (32 ms later), `session_resume pc=0x08800004` |
| No restart | Same process, one `session_start` | Pass: PID 11201 before and after; no `session_start elf=` line in the window |
| Guest frames | Frames continue within a bounded time | Pass: `guest_frame=120` before, then `180`, `240`, `300` after the resume (1.3 s to the first), all `draws=2834` with moving vertex counts |
| Progress | Battle state unchanged | Pass: screenshots show the same turn, unit (Giulio lv 9 Ace) and HP 1068/1068; screenshot hashes below |
| Audio | Dropped guest audio is counted as muted, no overruns or xruns | Pass: `muted=20`, `overruns=0`, `rejected=0`, `xruns=0`; `underrun` unchanged across the round trip |
| Scripted cycles | Three cycles resume with fresh frames and one start | Pass: `LIFECYCLE_OK`, 3 `session_resumed reason=foreground`, 1 `session_start elf=`, `muted=90`, `overruns=0` |

## Screenshot SHA-256

- Before background: `f1734328b8de46b173e64d9bbb1351fafc79abe4bdcf1f0cdec90ebde4df0a7b`
- After foreground: `7dd7592c20dfc8871ea004f054f20a7a05049d82d7a01ba4672f391fe990a234`

## Notes

- The stop is cooperative between dispatches, so `stop_pc` is a dispatch
  boundary; the resumed run continues from that exact pc.
- No game frames, dump hashes or generated content are stored with this
  record; the screenshots stay in the gitignored `build/qa/`.
- Process death while backgrounded still loses unsaved progress (no automatic
  snapshot on background); `docs/ANDROID.md` states that limit.

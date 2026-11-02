# scripts/android/AGENTS.md — device script catalog

`android/AGENTS.md` is the subsystem authority; this file is the catalog for
`scripts/android/`. All scripts use `set -euo pipefail`, target one device
via `ADB_SERIAL` (or `ADB` for a custom binary), and fail with named errors,
never silent success. Game frames and logs land under gitignored paths only.

## Rules

- Default handoff is `scripts/android/deploy.sh` (install + launch +
  frame proof). Never hand back a bare `install.sh`: installs never
  auto-start the app.
- Unlock the keyguard first; `run.sh` fails `device_locked` otherwise.
- Adding a script: document it here, in `docs/ANDROID.md`, in
  `docs/BUILDING.md` §7 if it is part of the from-scratch flow, and in the
  `test_building_covers_the_pipeline` phrase list in
  `tests/test_docs_building.py`.

## Scripts

- `build.sh [--release] [--clean] [--offline] [-P...]`: builds the debug
  (or release) APK. Machine paths via `-P` properties, never
  `local.properties`. Key props: `gungnir.sdkDir`, `gungnir.javaHome`
  (JDK 17–23), `gungnir.gameCorpus` (generated AOT output, never committed),
  `gungnir.abis`, `gungnir.o1Sources`, `gungnir.depsDir`. Prints the APK path;
  `BUILD SUCCESSFUL` is not enough, check the `APK:` line.
- `deploy.sh [APK] [SHOT]`: install + launch + proof. Expects a fresh APK
  (run `build.sh` first after native changes; `deploy.sh` never builds).
  Chains `install.sh` + `run.sh`, waits for flowing `guest_frame` lines, writes a proof screenshot
  (default `/tmp/opencode/scene/deploy-<stamp>.png`). Prints `DEPLOY_OK
  pid=… shot=…`; `deploy_failed` means the app is not executing.
  `SNAPSHOT_IN=<device path>` loads a pushed snapshot via the
  automation-only `snapshot_in` intent extra before the session starts
  (`RESUME_OK` names the resume; `snapshot_in_failed` / `resume_timeout`
  on a bad path or a load that never resumes).
- `install.sh [APK]`: `adb install -r` only. Does not start anything;
  always follow with `run.sh` (or use `deploy.sh`).
- `run.sh`: clears logcat, force-stops, starts `.GungnirActivity`, waits up
  to 60s for execution proof (`first frame` on fresh boot, `guest_frame`
  on a snapshot resume) or a bounded `session_stop` (`SESSION_OK` /
  `session_stop_not_first_flip`). Fails `device_locked` with the keyguard
  up, `session_start_failed`, or `frame_timeout`.
  `SNAPSHOT_IN=<device path>` starts with `--es snapshot_in` instead and
  requires `session_resume pc=` first (`RESUME_OK`, else
  `snapshot_in_failed` / `resume_timeout`).
- `logcat.sh`: `logcat -s gungnir:I SDL:V AndroidRuntime:E`. First filter
  for diagnosis: `guest_frame` (flowing?), `guest_stall`, `session_stop`,
  `atrac_decode`, `stall_thread`, `stall_msgpipe`.
- `screenshot.sh [OUT]`: `adb exec-out screencap -p` to `OUT` (default
  `build/qa/android/<stamp>.png`, gitignored). Fails `screenshot_failed`
  on an empty image; never commit game frames.
- `lifecycle.sh`: 3 pause/resume cycles against the running activity,
  asserting session continuity across each. Needs an unlocked device.
- `emulator.sh`: starts AVD `gungnir` (`$GUNGNIR_AVD`, port
  `$GUNGNIR_EMULATOR_PORT`, default 5554) with `-no-window -no-audio
  -no-boot-anim -gpu swiftshader_indirect -no-snapshot -wipe-data`, waits
  for `sys.boot_completed=1`. `-wipe-data` resets the AVD: re-push game
  data and snapshots afterwards.
- `qa-newgame.sh`: T13i device reproduction — title → NEW GAME →
  difficulty → ADVANCE → confirm, then watches for the null-object fault
  or a process kill. Evidence under `build/qa/android/` (gitignored).
- `qa-snapshot.sh`: T15e device QA — host snapshot → `adb push` to
  `.../files/snapshots/` → Menu load → play → menu save/restore →
  cold-boot restore → corrupt-file named error. Evidence under
  `build/qa/android/` (gitignored).

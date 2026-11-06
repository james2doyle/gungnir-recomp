# ANDROID — build, deploy and game-data intake

Status: T13b. The APK runs the shared session, HLE boot set, ATRAC decode,
SAS mixing and savedata chain, and presents guest GE frames. VERIFIED on the
arm64 phone: the title screen renders upright at the fitted aspect, music
plays through the AAudio backend, and touch input drives the title menu and
the difficulty selection (`docs/qa/2026-11-01-t13b-title.md`). The bounded
first-flip record is `docs/qa/2026-10-31-t13a-runtime.md`.

## Prerequisites

- JDK 17–23 (Gradle 8.11.1 does not support newer JDKs).
- Android SDK: platform 36, build-tools 36.0.0, NDK 28.2.13676358,
  CMake 3.22.1.
- Host `cmake` 3.22+, Ninja, `glslc` (Vulkan shaders), Python 3.
- `adb` and an arm64 device or an x86_64 emulator with Vulkan.

No `local.properties` is written or committed. Pass machine paths as `-P`
properties; `ANDROID_HOME`/`JAVA_HOME` are accepted as defaults.

## Build

```sh
scripts/android/build.sh \
  -Pgungnir.sdkDir="$ANDROID_HOME" \
  -Pgungnir.javaHome="$JAVA_HOME"
```

| Property | Meaning |
|---|---|
| `-Pgungnir.sdkDir=DIR` | Android SDK root |
| `-Pgungnir.javaHome=DIR` | JDK 17–23 |
| `-Pgungnir.depsDir=DIR` | fetched-dependency cache (default `build/android-deps`) |
| `-Pgungnir.o1Sources=LIST` | semicolon-separated sources compiled at `-O1` |
| `-Pgungnir.gameCorpus=DIR` | local generated AOT corpus (`generated_registry.cpp`, `recomp_units.hpp`, `generated_unit_*.cpp`); build output, never committed |
| `-P<name>=<value>` | any other Gradle project property |

`build.sh` validates the pinned SDK components and JDK, fetches SDL3
`release-3.4.18` plus the pinned libadrenotools and liblinkernsbypass sources
(SHA-256 pinned; `--offline` requires the archives). Volk is vendored
source, not fetched. The script then builds the debug APK. Release builds use
`--release`; no release signing is configured.

The runtime target compiles at `-O2 -fno-lto` even in debug. Named units can
be dropped to `-O1` through `-Pgungnir.o1Sources`; the choice is visible in
`android/app/.cxx/**/compile_commands.json` and in the Gradle configure log.

## Deploy

```sh
ADB_SERIAL=<serial> scripts/android/deploy.sh   # install + launch + guest_frame proof (preferred)
ADB_SERIAL=<serial> scripts/android/install.sh
ADB_SERIAL=<serial> scripts/android/run.sh        # waits for "session_stop" or "first frame"
ADB_SERIAL=<serial> scripts/android/logcat.sh     # gungnir/SDL/crash tags
ADB_SERIAL=<serial> scripts/android/screenshot.sh # PNG under build/qa/android/
ADB_SERIAL=<serial> scripts/android/lifecycle.sh  # 3 pause/resume cycles
ADB_SERIAL=<serial> scripts/android/qa-startup.sh # 10 cold boots, fails on any startup crash
```

`deploy.sh` chains install, launch, and verification: it fails loudly unless
guest frames are flowing and leaves a proof screenshot (default under
`${TMPDIR:-/tmp}/gungnir-temp/scene/`, override with a second argument). Installs never
auto-start the app, so a bare `install.sh` always needs a `run.sh` after it;
`deploy.sh` makes that ordering structural instead of relying on memory.

`run.sh` fails with `device_locked` when the keyguard is up. The screenshot
path is gitignored; do not commit game frames.

Widescreen follows the window by default (`--es widescreen
adaptive|off|16:9|21:9|32:9`): the frame widens to the display aspect, the
perspective scene is widened without zooming, and full-frame 2D effects
(backdrop, fades) stretch with it while the HUD keeps its pixel size and stays
centred. `off` renders the native 480x272 frame; the aspect caps bound the
widening for very wide windows. The boot log states the decision:
`widescreen mode=adaptive scale=1.3001 virtual=624x272`.

Presentation layout (`--es display_layout fit|stretch|crop`) remains a QA-only
switch; it has no settings row while adaptive widescreen already fills the
display, because the three policies coincide there. `fit` keeps the guest aspect with bars, `stretch` fills the display and
distorts, `crop` fills it uniformly and cuts ~10% off the top and bottom of the
image. None of them changes what the guest renders, and none is recorded in a
snapshot.

`qa-startup.sh` repeats the cold boot (`RUNS`, default 10) and fails on the
first launch that does not reach its first frame or that leaves a
`Fatal signal` in the crash buffer, keeping one logcat dump per run under
`build/qa/startup-<stamp>/`. It exists because the runtime shares the SDL
thread's stack (about 1 MiB, chosen by SDLActivity's Java thread), so an
oversized local frame crashes only on the device and only sometimes. Every
runtime target is compiled with `-Werror=frame-larger-than=16384`
(`cmake/BoundedStack.cmake`) to keep that class of bug out of the tree, and
`stack_space thread=main` in the boot log reports the margin actually left.

## CI build mode

`.github/workflows/ci.yml` builds a clean clone with zero game artifacts:
the Linux job runs `tools/deps.py verify`, the tracked-artifact scan, the
full synthetic suite (`ctest`, strict private-input mode via
`GUNGNIR_REQUIRE_PRIVATE_INPUT=1`), and `tools/repository_check.py`. The
Android job assembles the APK with an empty corpus (no `-Pgungnir.gameCorpus`,
no game paths) on the pinned SDK/NDK from `android/app/build.gradle.kts`.
A physical device is never required for CI. The first green run's URL is the
TC-13.2 evidence; record it in the T13d result, not in-repo.

## Runtime session and intent extras

A normal launch runs the guest continuously against `files/game/game.elf` and
presents guest GE frames through the swapchain. The bring-up and QA forms are
selected with intent extras:

```sh
adb shell am start -n com.gungnir.recomp/.GungnirActivity \
  --ei dispatch_limit 5000000 --ei wall_seconds 30 \
  --ez stop_at_flip true --ez pacing false
```

| Extra | Default | Meaning |
|---|---|---|
| `dispatch_limit` | 5,000,000,000 | hard upper bound on guest dispatches |
| `wall_seconds` | 0 (unbounded) | wall-clock bound for a bounded run |
| `stall_seconds` | 30 | report a guest thread that stops progressing for this long; 0 disables |
| `stop_at_flip` | false | stop at the first `sceDisplaySetFrameBuf` |
| `pacing` | true | pace the guest clock to real time (59.94 Hz vblanks) |
| `fast_forward` | false | start with fast-forward latched (QA form; 3x pacing, audio muted) |
| `debug_quad` | false | draw the synthetic test quad instead of the guest |
| `audio_smoke` | false | run the AAudio/SAS smoke instead of a game session |
| `audio_smoke_ms` | 500 | smoke duration |
| `dump_draws` | (none) | bounded guest-frame dump path; off unless named |
| `dump_presents` | (none) | bounded **presented**-frame dump path; off unless named |
| `dump_after_ms` | 0 | delay before either dump starts writing |
| `dump_frames` | 8 (max 32) | frame bound for either dump |
| `coverage` | 0 (off) | opt-in widescreen coverage measurement on every present; `--el coverage 1` enables it |

`coverage` is QA instrumentation, not a render requirement: it scans every draw
and fills a column bitmap on each present. It is off by default because running
it continuously in the production render loop is an avoidable load. Enable it
with `--el coverage 1` for a bounded coverage check; the periodic
`widescreen coverage=...` line appears only while it is enabled.

`dump_draws` writes the guest's own GE frames; `dump_presents` writes the
frames the presenter actually shows, including interpolated in-betweens. Both
share one verbatim format (draw key, through flag, vertex x/y/z and texture
coordinates), so an artifact that exists only between two guest frames can be
inspected offline instead of inferred from a video. Both are host-side, bounded
and opt-in: nothing is written unless the path extra is named, and neither
affects what is presented. The dumps live under the app's own files dir and are
never committed.

The bounded run logs, in order:

```text
audio_output_ready
session_start elf=...
first frame <width>x<height>
guest_frame=<n> draws=<n> vertices=<n> ge_gaps=<n>
audio_mixer pulled=<n> underrun=<n> submitted=<n> muted=<n> overruns=<n> rejected=<n> peak=<n> xruns=<n>
session_stop cause=<first_flip|dispatch_limit|wall_clock|missing_import|host_stop|guest_stop|scheduler_deadlock> dispatches=<n> flips=<n> imports_bound=<n> imports_total=<n> stop_pc=<addr>
session_stop_reason=<reason>
session_unbound=<library>:<nid>          (one line per unbound import)
guest_stall seconds=<n> flips=<n> inflight=<library::name|none> last_import=<library::name|none>
fast_forward active=<0|1> speed=<n>          (on every change)
```

`imports_unbound` is the number of counted gaps (the JSON report lists them);
`missing_import=1` means an unbound import executed and stopped the run with
its library and NID. `overruns` and
`rejected` describe the audio mixer (a rejected buffer is a programming error);
`xruns` is the AAudio underrun count from the driver.
`fast_forward active=1` means the pacing hold is running at 3x and the mixer
is dropping the guest's audio (`muted_buffers` grows, `overruns` does not).
A bounded run with `--ez fast_forward true` advances about three times as
much guest progress per wall second as an unpaced-at-1x run, which is the
host half of the device check; the phone measurement stays with QA.

`guest_stall` is emitted at most once per session by the frame watchdog: it
fires when no guest frame reaches the display for `stall_seconds`, which covers
both a handler blocked in a host wait and a guest loop that keeps dispatching
while polling a flag (the dispatch and wall-clock bounds never see either).
`inflight` names the handler running at that moment, `last_import` the most
recently completed one — a watch loop names itself there between calls.

## Per-title host patches (profile)

Per-title host patches are explicit, named, opt-in values the renderer/HLE may
use for a specific title (camera hook sites, transform inputs). They are **host
state**: never guest state, never part of snapshot identity, never written into
a snapshot. A profile change does not invalidate or alter an existing snapshot.

Values live in the gitignored, app-private path
`getFilesDir()/profiles/gungnir/local/host_patches.toml` — this is the same
policy as the dump-specific pins under `profiles/gungnir/local/`. No game data,
dump hashes or addresses are ever tracked. The file is optional: an absent
profile changes nothing and logs `profile absent` at boot.

The format is one `key = value` per line, `#` comments and blank lines allowed.
Every recognised key has a fixed type; adding one is a reviewed change to
`runtime/host/profile.cpp`. Recognised keys today:

| Key | Type | Meaning |
|---|---|---|
| `cam_projection` | u32 (dec or `0x` hex) | guest address of the projection-matrix builder |
| `cam_setup` | u32 (dec or `0x` hex) | guest address of the camera setup routine |
| `widescreen_fov_scale` | float | widescreen FOV scale for the camera hook |

Unvalidated values are **refused, never best-efforted**: an unknown key
(`profile_invalid: unknown profile key '…'`), a malformed value, a type
mismatch (e.g. a float in an address slot), a duplicate key, or an empty value
produce a named `profile_invalid` error and **no entry is applied**. Nothing is
silently defaulted into existence. Every applied entry is logged at boot as
`profile key=value` followed by `profile applied=<n>`, so a device boot shows
exactly which host patches are active.

## Game data

Two supported paths converge on `getExternalFilesDir()/game/`:

1. **adb push**

   ```sh
   adb push game.iso \
     /sdcard/Android/data/com.gungnir.recomp/files/game/game.iso
   ```

2. **SAF picker**: when no valid ISO is found, the app auto-shows the
   SAF file picker (Yakumo-style first run). The picker copies the chosen
   file to `.../files/game/imported.iso`, the native side verifies it and
   prepares `files/game/game.elf` on-device from the ISO's
   `PSP_GAME/SYSDIR/EBOOT.BIN` (DISC_ID + `~PSP` tag check, AES-128-CBC
   with the public PSP key tables), then starts the session. Picking the
   ISO is enough to start the game; no separate `game.elf` push is needed.
   A manually pushed `game.elf` (from the host intake) is still honoured
   when present, and is skipped when the installer already prepared one.

ADB parity (no picker): push the ISO and the app prepares `game.elf` on
the next start. The old manual module push remains supported:

```sh
adb push build/game/modules/SYSDIR__EBOOT.BIN.decrypted.elf \
  /sdcard/Android/data/com.gungnir.recomp/files/game/game.elf
```

The APK is built with the matching AOT corpus
(`-Pgungnir.gameCorpus=build/generated`, the output of the codegen step in
[`BUILDING.md`](BUILDING.md)); without one, the session fails with
`game_corpus_missing` instead of running empty. Synthetic-corpus builds use
the same property with the fixture corpus and never touch the real disc. The NID registry is
packaged from `configs/nids.csv` into app storage as `runtime/nids.csv`. The
memory stick is app-private at `getFilesDir()/psp/` (`PSP/SAVEDATA/...`).

Both paths must satisfy the published disc identity: size `262766592` and
CRC32 `183F3EDC`. A mismatch blocks use with a named log line
(`identity_size_mismatch`, `identity_crc_mismatch`, `game_data_open_failed`).
A corrupt candidate is skipped when another valid candidate exists.

The app unpacks `assets/runtime/version.txt` into
`getFilesDir()/runtime/version.txt` on first run and rewrites it only when the
packaged version changes.

## Drivers

The **Drivers** button (top-right) lists the system driver plus imported
arm64 driver packages. **Import zip** uses the system document picker; the zip
is copied into app-private storage, validated by the native inspector and
installed under `filesDir/drivers/<id>/`. A long press on a row confirms
delete. Selection is written to `filesDir/drivers/selection.txt` and takes
effect on the next start; **Driver info** shows the active driver and Vulkan
version reported by the running session. Zips dropped in
`getExternalFilesDir()/drivers/` are imported at startup. On x86_64 the dialog
reports that custom drivers require arm64 and keeps the system driver. See
[`ANDROID_DRIVERS.md`](ANDROID_DRIVERS.md) for the package contract.

## Audio

The runtime links an AAudio low-latency 44.1 kHz stereo backend. Game audio
reaches it through the mixer in `runtime/audio/audio_mixer.cpp`: every
`sceAudio` channel is summed onto one timeline, with a 46 ms lead, counted
resyncs when a channel falls behind and counted restarts when the timeline
outgrows the ring. `audio_mixer` logs the peak and the health counters.

ATRAC tracks are decoded locally; `sceAtracDecodeData` charges the media-engine
frame time on every attempt so the sound thread yields to the guest. SAS VAG
voices (including the long ADSR controls) render into guest memory before the
game submits them. Noise voices are silent and reverb is stored but not mixed;
both are named gaps in [`AUDIO.md`](AUDIO.md).

A backend smoke can be run without touching the UI:

```sh
adb shell am force-stop com.gungnir.recomp
adb shell am start -n com.gungnir.recomp/.GungnirActivity --ez audio_smoke true
adb logcat -d | grep audio_smoke
```

The native smoke opens AAudio, submits a 440 Hz tone and a synthetic VAG
through `SasCore`, then logs `audio_smoke ok/vag/pulled/underrun/xruns`. It is
for backend bring-up, not a soak. The title/audio device evidence is
`docs/qa/2026-11-01-t13b-title.md`; the backend record is
`docs/qa/2026-10-31-t12a-audio.md`.

## Input

Touch overlay, the Menu settings surface, layout JSON, keyboard/mouse maps,
gamepad profiles/remap and touch haptics are documented in
[`docs/INPUT.md`](INPUT.md). The physical-device evidence is in
`docs/qa/2026-10-31-t11-*.md`.

## Lifecycle and orientation

The Activity is fixed to landscape (`LandscapeLeft`), so user rotation locks
do not rotate the app. Entering the background releases held input, asks
`SessionLifecycle` for a stop, pauses AAudio, mutes the mixer for the whole
background window and logs `lifecycle_paused`; returning to the foreground
resumes the audio stream, recreates the `VkSurfaceKHR`/swapchain
(`lifecycle_resumed`) and then relaunches the worker from the pc it stopped at
(`session_resumed reason=foreground`).

The guest is stopped between dispatches, never mid-dispatch, and the decision
lives in `runtime/session/session_lifecycle.*` (host unit tests:
`tests/session/session_lifecycle_tests.cpp`). A live session is stopped once; a
run that ended for its own reason (dispatch or wall-clock bound, failure, the
stall watchdog) is **not** revived — that is logged as
`session_resume_skipped reason=own_stop cause=...`. The resume waits for the
new surface and shares the snapshot mutex with the menu save/load/reset paths.
`GameSession::request_resume` rebases the pacing hold, so a resumed run
continues at normal speed instead of racing to make up the pause.

One `session_start` line per launch is the invariant: a background/foreground
round trip keeps the process, the guest memory and the fast-forward latch. If
Android kills the process while backgrounded, unsaved progress is lost — there
is no automatic snapshot on background; use the menu's Save state first.

A failed render is retried up to three times, and a failed surface recreation
is logged as `surface_recreate_failed` and retried after 500 ms.

## Frame interpolation

The Settings overlay offers `Experimental frame interpolation: On/Off` (off by
default, labelled experimental while the shape and depth rules are still under
device review,
persisted in the `gungnir` preferences and pushed to native through
`HostBridge.setFrameInterpolation`). A `frame_interpolation=1` intent extra
selects it for a bounded QA run without touching the UI.

It is presentation only: the guest still runs at its own rate, and the option
is host state, never part of a snapshot. The periodic log reports
`interpolation active=<0|1> interpolated=<n> held=<n> skipped_ticks=<n>` next to
the present counters, so a QA log proves which mode produced the frames.

## Emulator notes

Use `scripts/android/emulator.sh` for a reproducible headless run. It starts
the `gungnir` AVD on `emulator-5554` with
`-no-window -no-audio -no-boot-anim -gpu swiftshader_indirect -no-snapshot
-wipe-data -netdelay none -netspeed full`, then waits for
`sys.boot_completed=1`. `-wipe-data` clears the AVD each start, so re-push
the user ISO and any driver fixtures. The x86_64 image needs Vulkan; the
chooser uses SwiftShader. Unlock/dismiss the keyguard before `run.sh` or
`lifecycle.sh`, otherwise the scripts stop with `device_locked`. Emulator
frames are 640x320 in the current AVD; the phone renders 3063x1335.

The emulator is for early feature iteration. A device-facing feature sprint
closes only after manual QA on the physical arm64 phone (see `AGENTS.md` and
`.dex/plan.md` §8.3); emulator evidence supplements it.

## Named failures

| Log / stderr | Meaning |
|---|---|
| `missing_sdk`, `missing_sdk_component` | SDK root or pinned component absent |
| `missing_jdk`, `unsupported_jdk` | JDK missing or outside 17–23 |
| `missing_sdl3_java`, `sdl_source_missing` | SDL3 not materialized |
| `missing_apk` | no APK at the expected output path |
| `device_locked` | keyguard is up |
| `game_data_unavailable: <reason>` | no/invalid ISO; reason is the identity error |
| `affinity_unavailable`, `priority_unavailable` | tuning denied; boot continues |
| `audio_output_unavailable` | AAudio could not open; the game session uses a named silent sink |
| `game_corpus_missing` | user-supplied corpus dir malformed (missing registry); leftover from strict-corpus builds — current builds log `interpreter_only_build` and run the interpreter fallback |
| `interpreter_only_build` | APK built without `-Pgungnir.gameCorpus`; guest runs on the interpreter fallback (release configuration), never refuses |
| `game_elf_missing` | `game.elf` is not in `files/game/` and the installer could not prepare it from the ISO; see `installer_failed` |
| `installer_failed: <reason>` | the on-device installer rejected the ISO (DISC_ID, tag or EBOOT header); the reason is player-actionable |
| `game_elf_invalid_magic` | `game.elf` is not an ELF file; intake output was not copied |
| `session_elf_missing`, `session_nids_missing` | the session inputs are absent |
| `session_audio_sink_missing` | no host audio sink was installed; programming error |
| `session_start_failed: <reason>` | the shared session refused to start; the reason is named |
| `session_error: <reason>` | the guest run failed after starting |
| `surface_recreate_failed` | swapchain surface could not be rebuilt; retried |
| `driver_fallback: <reason>` | a selected custom driver failed; the system driver is used |
| `stack_space thread=<name> unavailable` | the thread stack cannot be queried; the margin is unknown, boot continues |
| `unknown_display_layout: <text>` | `--es display_layout` named a policy that does not exist; the layout stays as it was |
| `driver_selection_invalid` | the persisted selection file is malformed; system is used |
| `driver_import_failed: <reason>` | a driver zip was rejected; no selection change |
| `driver_remove_failed: <reason>` | a driver directory could not be removed |
| `driver_hook_library_missing` | the APK is missing a libadrenotools hook library |
| `driver_storage_unavailable` | the private driver directory is missing or unreadable |
| `driver_abi_unsupported` | this ABI is system-only; no custom driver is opened |
| `driver_custom_open_failed` | Android diagnostic: libadrenotools returned null |
| `driver_linkernsbypass_unavailable` | Android diagnostic: the linker bypass failed |
| `driver_custom_symbol_missing` | Android diagnostic: the driver has no `vkGetInstanceProcAddr` |
| `vulkan_loader_unavailable` | no system Vulkan loader could be opened |
| `render_failed` | render failed; retried up to three times |

## Regression evidence

Each mutation was applied locally and the named gate failed for that reason:

| Mutation | Failing check |
|---|---|
| Gradle wrapper SHA replaced with zeros | `test_wrapper_is_sha_pinned` |
| CRC comparison removed from the game-data gate | `game_data_tests`: “CRC mismatch is named” |
| Per-unit `-O1` source property removed | `test_android_build`: `-O1` missing from `vertex.cpp` |

## Current limits

- The title screen and menu work on the arm64 device; confirming a difficulty
  currently faults in the new-game initialisation path (tracked work, not a
  title/menu failure).
- Frame production is bound by guest execution speed on the phone (~20 fps at
  the title); the guest clock is paced, but full-speed play is hardening work.
- MPEG/PMF movie playback is deferred: the opening movie is skipped with a
  counted gap and boot continues.
- Game audio is fed through the 44.1 kHz mixer to AAudio; if AAudio cannot
  open, the named silent sink is used and the failure is logged.
- SAS noise voices are silent and reverb is stored but not mixed.
- Savedata is mounted under `files/psp/PSP/SAVEDATA`; LOAD/SAVE/SIZES work,
  while the picker, LIST and DELETE remain named gaps. The save tree is not
  bundled and must be pushed by the user.
- Custom Adreno drivers are complete for arm64/API 28+; packages are
  user-supplied and never bundled. See
  [`ANDROID_DRIVERS.md`](ANDROID_DRIVERS.md) for the contract and named gaps.
- The affinity heuristic uses the highest allowed CPU, not topology/frequency.
- Debug APK only; release signing is not configured.

Manual QA evidence: [`docs/qa/2026-11-01-t13b-title.md`](qa/2026-11-01-t13b-title.md)
(title, audio, touch menu) and
[`docs/qa/2026-10-30-t9-android-shell.md`](qa/2026-10-30-t9-android-shell.md)
(shell and first frame).

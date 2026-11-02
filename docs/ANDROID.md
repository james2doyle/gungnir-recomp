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
```

`deploy.sh` chains install, launch, and verification: it fails loudly unless
guest frames are flowing and leaves a proof screenshot (default under
`/tmp/opencode/scene/`, override with a second argument). Installs never
auto-start the app, so a bare `install.sh` always needs a `run.sh` after it;
`deploy.sh` makes that ordering structural instead of relying on memory.

`run.sh` fails with `device_locked` when the keyguard is up. The screenshot
path is gitignored; do not commit game frames.

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
| `debug_quad` | false | draw the synthetic test quad instead of the guest |
| `audio_smoke` | false | run the AAudio/SAS smoke instead of a game session |
| `audio_smoke_ms` | 500 | smoke duration |

The bounded run logs, in order:

```text
audio_output_ready
session_start elf=...
first frame <width>x<height>
guest_frame=<n> draws=<n> vertices=<n> ge_gaps=<n>
audio_mixer pulled=<n> underrun=<n> submitted=<n> overruns=<n> rejected=<n> peak=<n> xruns=<n>
session_stop cause=<first_flip|dispatch_limit|wall_clock|missing_import|host_stop|guest_stop|scheduler_deadlock> dispatches=<n> flips=<n> imports_bound=<n> imports_total=<n> stop_pc=<addr>
session_stop_reason=<reason>
session_unbound=<library>:<nid>          (one line per unbound import)
guest_stall seconds=<n> flips=<n> inflight=<library::name|none> last_import=<library::name|none>
```

`imports_unbound` is the number of counted gaps (the JSON report lists them);
`missing_import=1` means an unbound import executed and stopped the run with
its library and NID. `overruns` and
`rejected` describe the audio mixer (a rejected buffer is a programming error);
`xruns` is the AAudio underrun count from the driver.

`guest_stall` is emitted at most once per session by the frame watchdog: it
fires when no guest frame reaches the display for `stall_seconds`, which covers
both a handler blocked in a host wait and a guest loop that keeps dispatching
while polling a flag (the dispatch and wall-clock bounds never see either).
`inflight` names the handler running at that moment, `last_import` the most
recently completed one — a watch loop names itself there between calls.

## Game data

Two supported paths converge on `getExternalFilesDir()/game/`:

1. **adb push**

   ```sh
   adb push game.iso \
     /sdcard/Android/data/com.gungnir.recomp/files/game/game.iso
   ```

2. **SAF picker**: when no valid ISO is found, the app overlays a
   “Select Gungnir ISO” button. The picker copies the chosen file to
   `.../files/game/imported.iso` and re-verifies it.

The bounded game session also needs the decrypted main module produced by the
user's own intake run. Push it as `game.elf`:

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
do not rotate the app. Entering the background releases held input, requests a
session stop, pauses AAudio and logs `lifecycle_paused`; returning to the
foreground resumes the audio stream and recreates the `VkSurfaceKHR`/
swapchain (`lifecycle_resumed`). The guest session itself is not restarted
after a background stop — a named lifecycle gap for the manual QA matrix. A
failed render is retried up to three times, and a failed surface recreation is
logged as `surface_recreate_failed` and retried after 500 ms.

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
| `game_corpus_missing` | APK built without `-Pgungnir.gameCorpus`; rebuild with the user's generated corpus |
| `game_elf_missing` | `game.elf` is not in `files/game/`; push the user-generated decrypted module |
| `game_elf_invalid_magic` | `game.elf` is not an ELF file; intake output was not copied |
| `session_elf_missing`, `session_nids_missing` | the session inputs are absent |
| `session_audio_sink_missing` | no host audio sink was installed; programming error |
| `session_start_failed: <reason>` | the shared session refused to start; the reason is named |
| `session_error: <reason>` | the guest run failed after starting |
| `surface_recreate_failed` | swapchain surface could not be rebuilt; retried |
| `driver_fallback: <reason>` | a selected custom driver failed; the system driver is used |
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

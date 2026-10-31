# ANDROID — build, deploy and game-data intake

Status: T9 shell. The APK currently renders the T7 synthetic Vulkan triangle,
not Gungnir. Game-data intake, identity checking, asset unpacking and
lifecycle recovery are implemented; the real runtime chain lands with T13.

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
  -Pgungnir.sdkDir=/opt/android-sdk \
  -Pgungnir.javaHome=/usr/lib/jvm/java-21-openjdk
```

| Property | Meaning |
|---|---|
| `-Pgungnir.sdkDir=DIR` | Android SDK root |
| `-Pgungnir.javaHome=DIR` | JDK 17–23 |
| `-Pgungnir.depsDir=DIR` | fetched-dependency cache (default `build/android-deps`) |
| `-Pgungnir.o1Sources=LIST` | semicolon-separated sources compiled at `-O1` |
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
ADB_SERIAL=30c1f958 scripts/android/install.sh
ADB_SERIAL=30c1f958 scripts/android/run.sh        # waits for "first frame"
ADB_SERIAL=30c1f958 scripts/android/logcat.sh     # gungnir/SDL/crash tags
ADB_SERIAL=30c1f958 scripts/android/screenshot.sh # PNG under build/qa/android/
ADB_SERIAL=30c1f958 scripts/android/lifecycle.sh  # 3 pause/resume cycles
```

`run.sh` fails with `device_locked` when the keyguard is up. The screenshot
path is gitignored; do not commit game frames.

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

## Input

Touch overlay, the Menu settings surface, layout JSON, keyboard/mouse maps,
gamepad profiles/remap and touch haptics are documented in
[`docs/INPUT.md`](INPUT.md). The physical-device evidence is in
`docs/qa/2026-10-31-t11-*.md`.

## Lifecycle and orientation

The Activity is fixed to landscape (`LandscapeLeft`), so user rotation locks
do not rotate the app. Background/foreground pauses and resumes a synthetic
SDL audio tone, recreates the `VkSurfaceKHR`/swapchain on resume, and retries
a failed render up to three times. A failed surface recreation is logged as
`surface_recreate_failed` and retried after 500 ms. Actual game audio is T12.

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
| `audio_unavailable` | no audio device; the probe continues silently |
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

- No game rendering yet; the title screen is T13.
- Audio is a lifecycle tone, not game audio (T12).
- Custom Adreno drivers are complete for arm64/API 28+; packages are
  user-supplied and never bundled. See
  [`ANDROID_DRIVERS.md`](ANDROID_DRIVERS.md) for the contract and named gaps.
- The affinity heuristic uses the highest allowed CPU, not topology/frequency.
- Debug APK only; release signing is not configured.

Manual QA evidence: [`docs/qa/2026-10-30-t9-android-shell.md`](qa/2026-10-30-t9-android-shell.md).

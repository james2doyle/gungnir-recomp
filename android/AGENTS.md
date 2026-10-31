# android/AGENTS.md — Android subsystem rules

Root `AGENTS.md` is authoritative; this file adds Android-specific rules for
`android/`, `runtime/platform/android/`, `runtime/platform/adreno/`, the
Android CMake files, and `scripts/android/`.

## Toolchain (do not drift)

- AGP 8.10.1, Gradle 8.11.1 (SHA-pinned wrapper), JDK 17–23.
- NDK 28.2.13676358, CMake 3.22.1, build-tools 36.0.0.
- compileSdk 36, minSdk 26, targetSdk 35, `arm64-v8a` + `x86_64`,
  `c++_static`, C++20.
- No `local.properties`; pass machine paths as `-P` properties.
- SDL3 is fetch-only; never ship `libSDL3.so`.

## Build, deploy and game data

- Use `scripts/android/{build,install,run,logcat,screenshot,lifecycle}.sh`.
- `build.sh` validates the pinned SDK/JDK, fetches SHA-pinned SDL3,
  libadrenotools and liblinkernsbypass, and builds the debug APK. Volk is
  vendored; no third-party driver binaries are bundled.
- Game data: SAF or adb push into `getExternalFilesDir()/game/`; enforce size
  `262766592` and CRC32 `183F3EDC`. No `MANAGE_EXTERNAL_STORAGE`.

## Emulator vs physical device

- Emulator is fine during initial feature bring-up and fast iteration.
- It never closes a device-facing feature sprint. At sprint end, run manual
  QA on the physical arm64 Adreno phone and record model/API/GPU, build SHA,
  steps, expected/actual and sign-off in `docs/qa/`; three cold boots.
- Reproducible run: `scripts/android/emulator.sh` starts AVD `gungnir` on
  `emulator-5554` with
  `-no-window -no-audio -no-boot-anim -gpu swiftshader_indirect
  -no-snapshot -wipe-data -netdelay none -netspeed full`, then waits for
  `sys.boot_completed=1`. `-wipe-data` resets the AVD; re-push user data.
- Unlock the keyguard before `run.sh`/`lifecycle.sh` (`device_locked`).
- Target devices explicitly with `ADB_SERIAL=emulator-5554` or the phone
  serial.

## Driver loader

- Driver zips are user-supplied, app-private, never committed or bundled.
- `useLegacyPackaging = true`; hook libraries live in `nativeLibraryDir`.
- System driver is the default. Custom drivers require `arm64-v8a` and
  API 28+; `x86_64` is system-only and must not import.
- Never `dlopen` from external storage. Fail closed with named errors; see
  `docs/ANDROID_DRIVERS.md` for the package contract and error list.
- Selection lives in `filesDir/drivers/selection.txt` and applies on the
  next start.

## Testing

- Host gates: `test_android_build`, `test_android_app`, `test_game_data`,
  `test_affinity`, `test_driver_package`, `test_vulkan_loader`,
  `test_driver_store`, `test_android_suite`.
- Automated tests and CI never substitute for the physical-device QA gate.
- Start every task with reference discovery: `gh search code
  "<api-or-symbol>"` (add `--extension cpp` for native work), then
  `gh search issues "<api-or-symbol>"`. Record the references used.

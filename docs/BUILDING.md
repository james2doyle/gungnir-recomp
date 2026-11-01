# BUILDING — from an empty machine to the title screen

This guide takes a new contributor from a fresh checkout to the game's title
screen on an Android device. You need your own legally obtained copy of the
disc; the repository contains no game data, no decrypted executables, no keys
and no generated recompiled code.

Everything derived from your disc is written under the gitignored `build/`
directory. Nothing in this guide writes game data into the repository.

## 0. What you supply

| Item | Why |
|---|---|
| Your own UMD **ISO** | the pipeline's only disc input; a `.chd` is converted first (see [INTAKE.md](INTAKE.md)) |
| A decrypt tool | runs out of process; the repository ships no decryption code or keys |

The intake gate verifies the disc identity itself; see [INTAKE.md](INTAKE.md)
for the recorded size/CRC32 and the conversion step. A wrong image is refused
with a named error instead of being processed.

## 1. Prerequisites

Linux x86_64 is the supported host. A C++20 toolchain, CMake, Ninja, Python
and the Android SDK/NDK are required:

- Git, Python 3.11+, CMake 3.22+, Ninja, pkg-config, `glslc` (Vulkan shaders).
- A C++20 compiler (GCC 13+ or Clang 17+).
- JDK 17–23 for Gradle 8.11.1.
- Android SDK: platform 36, build-tools 36.0.0, NDK 28.2.13676358, CMake
  3.22.1.
- `adb`, and either an arm64 device or the reproducible x86_64 emulator.

Point the environment at your SDK and JDK; no `local.properties` is written:

```sh
export ANDROID_HOME="$HOME/Android/Sdk"
export JAVA_HOME="/usr/lib/jvm/java-21-openjdk"
```

Use whatever paths exist on your machine. The commands below pass them to the
build script as `-P` properties, so the repository stays machine-independent.

## 2. Checkout and dependency verification

```sh
git clone <your-fork-url> gungnir-recomp
cd gungnir-recomp
python3 tools/deps.py verify
```

`tools/deps.py verify` checks every vendored tree against its pinned digest.
Fetch-based entries (SDL3, libadrenotools) print `not materialized` until the
Android build downloads them into `build/android-deps/`; the build script
verifies their archive digests before use.

## 3. Disc intake and decryption

Convert a CHD once with an external tool, then run the intake pipeline:

```sh
python3 tools/intake/intake.py run --iso game.iso --out build/game
```

Intake verifies the disc identity, extracts `PSP_GAME/`, validates
`PARAM.SFO`, inventories the modules and writes `build/game/manifest.json`.
Encrypted modules are decrypted out of process. Put your decrypt tool on
`PATH` as `pspdecrypt`, or name it explicitly:

```sh
python3 tools/intake/intake.py run --iso game.iso --out build/game \
  --decrypt-tool 'pspdecrypt -o {out} {in}'
```

The decrypted main module lands at
`build/game/modules/SYSDIR__EBOOT.BIN.decrypted.elf`. Named failures
(`DECRYPT_REQUIRED`, `DECRYPT_TOOL_FAILED`, `IDENTITY_MISMATCH`, ...) are
listed in [INTAKE.md](INTAKE.md).

## 4. Static analysis and AOT generation

```sh
python3 tools/profile/gungnir_profile.py analyze \
  --config profiles/gungnir/config.toml \
  --manifest build/game/manifest.json \
  --out build/profile/analysis

python3 tools/profile/gungnir_profile.py codegen \
  --config profiles/gungnir/config.toml \
  --analysis build/profile/analysis/analysis.json \
  --out build/generated
```

`analyze` runs the vendored analyzer, reconciles it with the intake manifest,
enforces the census gates and writes a deterministic
`build/profile/analysis/analysis.json`.
`codegen` emits the chunked AOT translation units and
`build/generated/manifest.json`. Re-running codegen over an unchanged corpus
rewrites nothing. Analysis and generated code stay under `build/` and are
never committed.

## 5. Host build and tests

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 -m unittest discover -s tests -p 'test_*.py'
```

Tests that need your disc or the generated corpus skip with `exit 77` when
the private inputs are absent; they never pass silently. The suite costs
minutes per run: always run it through the wrapper, which keeps the full
output in a timestamped log under `build/test-logs/` (gitignored) and
reprints failure names at the end — never truncate its pipe, read the log:

```sh
tools/run_python_suite.sh
``` The full suite and
the IP gate must be green before you build the APK:

```sh
python3 tools/repository_check.py
```

## 6. Android build

The APK needs your generated corpus at configure time:

```sh
scripts/android/build.sh \
  -Pgungnir.sdkDir="$ANDROID_HOME" \
  -Pgungnir.javaHome="$JAVA_HOME" \
  -Pgungnir.gameCorpus=build/generated \
  -Pgungnir.abis=arm64-v8a
```

Add `--offline` once `build/android-deps/` is populated to require the pinned
archives instead of downloading them. `--release` assembles a release APK
(unsigned; release signing is not configured). Other `-P` properties and the
SDK/JDK validation rules are in [ANDROID.md](ANDROID.md).

Expected last lines:

```text
BUILD SUCCESSFUL
APK: android/app/build/outputs/apk/debug/app-debug.apk
```

## 7. Install and run

```sh
ADB_SERIAL=<your-device-serial> scripts/android/install.sh
adb push build/game/modules/SYSDIR__EBOOT.BIN.decrypted.elf \
  /sdcard/Android/data/com.gungnir.recomp/files/game/game.elf
ADB_SERIAL=<your-device-serial> scripts/android/run.sh
```

`scripts/android/run.sh` waits for the first guest frame or a bounded session
stop and fails
with `device_locked` while the keyguard is up. Push the ISO to
`.../files/game/game.iso` once, or use the in-app picker. The memory stick is
app-private under `files/psp/`.

Expected on device, once the session starts:

```text
session_start elf=...
first frame <width>x<height>
guest_frame=<n> draws=<n> vertices=<n>
audio_mixer pulled=<n> underrun=<n> peak=<n> xruns=<n>
```

To capture evidence, use `scripts/android/screenshot.sh`; it writes a
gitignored image under `build/qa/android/`. Never commit game frames.

## 8. Offline and clean rebuilds

```sh
scripts/android/build.sh --offline --clean \
  -Pgungnir.sdkDir="$ANDROID_HOME" \
  -Pgungnir.javaHome="$JAVA_HOME" \
  -Pgungnir.gameCorpus=build/generated
```

`--offline` never downloads; `--clean` runs the Gradle clean task first.
Deleting `build/` discards the extracted game tree, the analysis, the
generated corpus and the APK, so intake and codegen must run again.

## 9. Named failures

Every failure is a named message, never a silent skip. The names by stage:

| Stage | Names |
|---|---|
| Intake | `CHD_NOT_SUPPORTED`, `IDENTITY_MISMATCH`, `ISO_INVALID`, `DISC_ID_MISMATCH`, `SFO_MISSING`, `DECRYPT_REQUIRED`, `DECRYPT_TOOL_FAILED`, `ENCRYPTED_EXECUTABLE`, `ELF_INVALID`, `PRX_SHAPE` |
| Analysis / codegen | exit code 3 with the named census or discovery gate that failed |
| Android build | `missing_sdk`, `missing_sdk_component`, `missing_jdk`, `unsupported_jdk`, `missing_cmake`, `missing_sdl3_java`, `missing_apk` |
| Device session | `device_locked`, `game_data_unavailable`, `game_corpus_missing`, `game_elf_missing`, `session_start_failed`, `session_stop_reason` |
| Rendering / drivers | `render_failed`, `driver_*` (see [ANDROID.md](ANDROID.md)) |

The full Android table, including every `driver_*` and Vulkan name, is in
[ANDROID.md](ANDROID.md). Intake details are in [INTAKE.md](INTAKE.md).

## 10. What is not implemented

- MPEG/PMF movie playback is deferred; the opening movie is skipped with a
  counted gap and boot continues.
- Savedata LOAD/SAVE/SIZES work; the save picker, LIST and DELETE are named
  gaps.
- The title and menu run on the device, but frame production is bound by
  guest execution speed on current hardware; full-speed play is hardening
  work, not a claim of this guide.
- Custom Adreno drivers, texture replacement and audio coverage limits are in
  [ANDROID_DRIVERS.md](ANDROID_DRIVERS.md), [TEXTURE_REPLACEMENT.md](TEXTURE_REPLACEMENT.md)
  and [AUDIO.md](AUDIO.md).

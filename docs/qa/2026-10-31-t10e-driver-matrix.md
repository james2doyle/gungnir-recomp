# QA — driver fail-closed matrix, real Turnip load and repository guard (T10e)

Date: 2026-10-31
App commit: `11137ce` (the APK was built from that tree).
APK: debug, `com.gungnir.recomp`, versionName `0.1.0`.
Sign-off: **PASS for TC-10.1–TC-10.4.** TC-10.2 is verified with a real
Rutabaga/Turnip package; v863.1 also loads.

## Devices

| Device | SoC / GPU | ABI | Android | Role |
|---|---|---|---|---|
| Xiaomi M2102K1AC (Mi 11 Ultra) | Snapdragon / Adreno | arm64-v8a | 14 (API 34) | primary phone |
| Android emulator `sdk_gphone64_x86_64` | SwiftShader / gfxstream | x86_64 | 17 (API 37) | non-Adreno path |

The phone was unlocked. No driver archive was committed; the real packages
were user-supplied and removed from the phone after the session. A locally
generated synthetic zip (valid ELF64 container, `meta.json`,
`libvulkan.so`) exercised the fail-closed paths only. Screenshots stay in
gitignored `build/qa/android/`.

## 1 — TC-10.1 system and fallback boots

Build and install:

```sh
scripts/android/build.sh \
  -Pgungnir.sdkDir=/opt/android-sdk \
  -Pgungnir.javaHome=/usr/lib/jvm/java-21-openjdk
ADB_SERIAL=30c1f958 scripts/android/install.sh
```

Three consecutive cold starts on the phone (force-stop, launch, wait for
`FRAME_OK`):

```text
boot1 driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
      info=Driver Build: 329cf4c2a7, I63533b1e29, 1675175164
      first frame 3063x1335
boot2 driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
      first frame 3063x1335
boot3 driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
      first frame 3063x1335
```

Emulator system path after install:

```text
I gungnir : driver_active=system name=gfxstream (SwiftShader driver)
            info=Mesa 26.2.0-devel api=4206592
I gungnir : first frame 640x320
```

A selected-but-unloadable driver (synthetic arm64 package with the real
hook directory) booted the system driver:

```text
I gungnir : driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver ...
E gungnir : driver_fallback: driver_load_failed
I gungnir : first frame 3063x1335
```

A selection whose hook directory was absent also fell back with a named
error:

```text
E gungnir : driver_fallback: driver_hook_library_missing
I gungnir : driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver ...
I gungnir : first frame 3063x1335
```

A selection whose private driver directory was absent:

```text
E gungnir : driver_fallback: driver_storage_unavailable
I gungnir : driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver ...
I gungnir : first frame 3063x1335
```

The x86_64 emulator rejects an injected custom selection before opening
anything:

```text
E gungnir : driver_fallback: driver_abi_unsupported
I gungnir : driver_active=system name=gfxstream (SwiftShader driver) ...
I gungnir : first frame 640x320
```

Expected: every failure names its cause and the app still presents a
system-driver frame. Actual: pass.

## 2 — TC-10.3 package and storage failures

Three zips were placed in `getExternalFilesDir()/drivers/` and imported
on the phone at startup:

```text
E gungnir : driver_import_failed: driver_zip_unsupported   (malformed zip)
I gungnir : driver_import_ok id=ea3e220c142a173c soname=libvulkan.so
E gungnir : driver_import_failed: driver_elf_machine       (x86_64 ELF)
I gungnir : driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver
I gungnir : first frame 3063x1335
```

The successful package wrote `package.properties` with `id`,
`display_name`, `vendor`, `driver_version`, `min_api=28` and `soname`;
the x86_64 package produced no directory. Selecting the synthetic
package wrote the expected selection file and produced the
`driver_load_failed` fallback above. The missing hook library and
missing private directory cases are recorded in section 1.

Expected: malformed content, wrong ABI, missing hook libraries and a
missing/unreadable driver directory are distinct named failures, and no
partial driver becomes active. Actual: pass.

## 3 — TC-10.4 repository guard

Red-first: five new `tests/test_repository_check.py` probes failed before
the rules were implemented (renamed zip, root `drivers/`, asset-side
`drivers/`, renamed zip in assets, source `jniLibs/`). After the change
all 13 repository tests pass, including the real work tree.

The gate now rejects:

- any file under root `drivers/`, `android/app/src/main/assets/drivers/`
  or `android/app/src/main/jniLibs/` by location;
- ZIP magic (`PK\x03\x04`, `PK\x05\x06`, `PK\x07\x08`) at offset 0, so a
  renamed archive is still caught;
- the existing ELF/PSP/CHD/PGF/PBP magic and forbidden-extension rules.

`.gitignore` now explicitly ignores the asset-side driver directory and
the source `jniLibs/` tree, with the rationale recorded next to the
driver-blob block. Work tree:

```text
repository_check: PASS — 300 file(s) clean, 17 ignore-rule probes intact
```

Mutation check: retaining the failed custom handle instead of closing it
(rather than falling back cleanly) made `loader_tests` fail with
`FAIL: failed custom handle is closed`, then the mutation was reverted.

## 4 — TC-10.2 real custom driver load

`Turnip-Rutabaga-v3.zip` (Rutabaga-26.10.10, Mesa 26.3.0-devel
git-b00479181d) imported from the drop folder and loaded on the phone:

```text
I gungnir : driver_import_ok id=45b92cecfd9a862d soname=libvulkan_freedreno.so
I hook_impl: loading custom driver: /data/user/0/com.gungnir.recomp/files/drivers/45b92cecfd9a862d/libvulkan_freedreno.so
I gungnir : driver_active=custom name=rutabaga Mesa driver
            info=Mesa 26.3.0-devel (git-b00479181d) api=4206951
I gungnir : first frame 3063x1335
```

Three consecutive cold starts produced the same custom-driver line and
first frame. `v863.1.zip` (Qualcomm Vulkan 1.4.295, soname
`vulkan.ad0863.so`, multiple driver libraries) also loaded:

```text
I gungnir : driver_active=custom name=Qualcomm Technologies Inc. Adreno Vulkan Driver
            info=Driver Build: ffdf890711, I896ce78af2, 1766558511 ... api=4198695
I gungnir : first frame 3063x1335
```

`driverName`/`driverInfo` come from `VK_KHR_driver_properties` after
instance creation; both packages reported a real version, so TC-10.2 is
pass.

The first real run failed with `driver_custom_open_failed` and no hook
logs. `driver_linkernsbypass=1` proved `liblinkernsbypass` had loaded;
the actual cause was that libadrenotools concatenates `customDriverDir`
and the soname without a separator, while the selection file writes
`driver_dir` without a trailing slash. The loader now appends the slash
before calling libadrenotools, and the host loader test asserts the
forwarded directory ends with `/`.

## Cleanup

The imported real driver directories, `selection.txt` and the drop zips
were removed from the phone; the emulator's injected selection was
removed. Both devices were left on the system driver and rebooted with
`driver_active=system` and a first frame.

## Host evidence

`python3 -m unittest discover -s tests -p 'test_*.py' -v` ran 125 tests
OK. `tests/test_vulkan_loader.py` covers the new preflight errors;
`tests/test_repository_check.py` covers the location and magic guards.
`python3 tools/repository_check.py` passes on the committed tree.

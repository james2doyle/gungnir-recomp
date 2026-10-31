# ANDROID_DRIVERS — custom Adreno Vulkan drivers

Status: T10 complete. The inspector, loader, UI, fail-closed matrix and
physical-device QA are in place. No driver package or binary is bundled with
this project. Evidence labels are recorded below.

Gungnir can render through the device's system Vulkan driver (the default) or
through a user-supplied Mesa Turnip-style ICD package. This file defines what
a package is, where it may live, how it is validated, and how failures behave.

## Where driver packages come from

This project does not host, mirror, sign, or recommend driver builds. Obtain a
package from the driver project's own release channel or your device vendor.
Mesa Turnip builds come from the Mesa Android/Turnip build process; Qualcomm
packages come from a vendor image or a community repack. Use a source you
trust: a driver zip is third-party native code running inside the app process
with the app's permissions. There is no signature check, update feed, or
compatibility guarantee.

A package is a `.zip`; the supported shapes are below. The app copies the zip
into app-private storage, validates it structurally, and never loads anything
from shared storage.

## Decision

Adopt [bylaws/libadrenotools](https://github.com/bylaws/libadrenotools) rather
than write a clean-room Vulkan ICD loader. `AGENTS.md` hard limit #0 forbids
reimplementing an existing format or loader without explicit user sign-off;
libadrenotools is the mature, BSD-2-Clause option used by PPSSPP, Dolphin,
Vita3K, Flycast, Strato and Azahar.

| Item | Value |
|---|---|
| Loader | `bylaws/libadrenotools` |
| Commit | `8fae8ce254dfc1344527e05301e43f37dea2df80` |
| Archive SHA-256 | `ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb` |
| License | BSD-2-Clause |
| Transitive | `bylaws/liblinkernsbypass` @ `aa3975893d83ef1bc84c321ec60c65fbf1287887` (BSD-2-Clause) |
| Local patches | none |

Both pins live in `dependencies.json`; the archives are fetched and verified
by the Android build and are never vendored, committed or bundled. The
submodule content is fetched separately because GitHub commit archives omit
it. Details: `third_party/libadrenotools/PROVENANCE.md` and
`third_party/liblinkernsbypass/PROVENANCE.md`.

## Package contract

A driver package is a `.zip` supplied by the user. Two shapes are accepted.

### 1. Turnip-style package (preferred)

`meta.json` at the zip root plus the ICD shared object. The schema is the
common Turnip/ADPKG metadata shape:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `schemaVersion` | int | yes | only `1` is accepted; missing or greater than 1 is rejected |
| `name` | string | yes | non-empty display name |
| `libraryName` | string | yes | ICD soname, a basename ending in `.so` (for example `vulkan.ad07XX.so`); path separators and `..` are rejected |
| `minApi` | int | no | lowest Android API; default `0`; the Drivers UI refuses a selection newer than the device API |
| `vendor` | string | no | shown as `unknown` when absent |
| `driverVersion` | string | no | shown as `unknown` when absent |
| `description` | string | no | shown in the detail view |
| `author` | string | no | attribution shown in the detail view |
| `packageVersion` | string | no | packaging revision, not the Vulkan version |

Additional files (driver configuration, notices) may be present; they are
extracted but never executed. The soname always comes from `libraryName`,
never from scanning the archive.

### 2. Plain package

No `meta.json`, and the zip contains **exactly one** root-level shared object
named `libvulkan.so`. The display name comes from the zip filename; `vendor`
and `driverVersion` are `unknown`; `minApi` is `0`. Any other plurality or
nesting is rejected (`driver_library_ambiguous` / `driver_library_missing`).
ADPKG is not accepted.

### Archive and ELF limits

- compressed archive at most 512 MiB, at most 4096 entries, at most 256 MiB
  per entry, at most 1 GiB uncompressed total;
- no absolute paths, no `..` components, no duplicate names, no encrypted
  entries, no unsupported compression methods;
- the ICD must be ELF64, little-endian, `ET_DYN`, `EM_AARCH64`, with a
  non-empty `.dynsym`; 32-bit, `EM_X86_64`, `ET_REL`/`ET_EXEC`, and truncated
  files are rejected before extraction.

### Inspector implementation (T10b)

`runtime/platform/adreno/package.{hpp,cpp}` performs inspection and atomic
install without loading code. A successful install also writes
`package.properties` into the private driver directory for the Java driver
list; it is metadata, not a driver file. The ZIP container is handled by miniz 3.1.2
(MIT); `meta.json` is parsed by nlohmann/json 3.12.0 (MIT); the content id uses
the adopted yakumo SHA-256. Both libraries are pinned in `dependencies.json`
and never hand-rolled. `inspect_driver_zip()` reads the whole archive into
memory, so the id and the validated bytes are the same snapshot;
`install_driver_zip()` extracts that snapshot into
`filesDir/drivers/.staging-<id>/` and renames it to `filesDir/drivers/<id>/`
only after every entry was written. A failure leaves neither a staged nor a
partially installed driver. `scan_drop_folder()` returns valid packages and
named per-file errors instead of silently skipping bad zips.

### Loader and dispatch (T10c)

`runtime/platform/adreno/loader.cpp` selects the Vulkan loader before any
instance exists. The system path tries `libvulkan.so.1`, then `libvulkan.so`
(Windows: `vulkan-1.dll`; macOS: the MoltenVK names) and never calls the
custom opener. Before calling libadrenotools it verifies that the private
driver directory is readable and that `nativeLibraryDir` contains all four
hook libraries (`driver_storage_unavailable` / `driver_hook_library_missing`).
`driver_dir` in the selection file has no trailing slash; the loader appends
one because libadrenotools concatenates the directory and soname directly.
The custom path then calls
`adrenotools_open_libvulkan(RTLD_NOW|RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
tmpLibDir, nativeLibraryDir, privateDriverDir, soname, nullptr, nullptr)` and
uses the returned `vkGetInstanceProcAddr`. If the custom path fails, the
requested error is retained and the system loader is opened instead; the app
logs `driver_fallback: <named error>` and boots on the system driver. No
Vulkan instance is created while the driver choice is unresolved. Android
diagnostics distinguish the stages: `driver_linkernsbypass_unavailable`
(linker bypass failed), `driver_custom_open_failed` (libadrenotools returned
null) and `driver_custom_symbol_missing` (no `vkGetInstanceProcAddr`).

Android arm64 uses the vendored Volk table for both paths; `volkLoadInstance`
and `volkLoadDevice` populate the entry points after creation. Desktop builds
keep the direct Vulkan link and do not compile Volk. After device creation the
backend queries `VK_KHR_driver_properties` when available and logs one
`driver_active=<system|custom> name=... info=... api=...` line; the UI reads
the same `VulkanDriverInfo`. The x86_64 emulator build is system-only and
does not package the hook libraries.

## Install and selection flow

1. **Import** — the user picks a zip with SAF (`ACTION_OPEN_DOCUMENT`) or
   drops it in `getExternalFilesDir()/drivers/`. The zip is copied into
   private storage first; nothing is ever `dlopen`ed from external storage.
2. **Validate** — T10b inspects `meta.json` and the ELF without loading the
   code. The stable id is the first 16 hex characters of the SHA-256 of the
   zip bytes, so re-importing the same package is idempotent.
3. **Stage and rename** — extract into `filesDir/drivers/.staging-<id>/`,
   then atomically rename to `filesDir/drivers/<id>/`. A failed import leaves
   no partial driver visible.
4. **Select** — default is **System driver**. Selecting an imported driver
   loads it once at startup; the choice is persisted. Delete removes the
   private directory and falls back to system.
5. **Report** — one startup logcat line names the active driver and the real
   Vulkan version obtained from `vkEnumerateInstanceVersion` and
   `VK_KHR_driver_properties` (`driverName` / `driverInfo`). The Drivers
   dialog asks the native side for the same values through
   `nativeDriverInfo()`.

On Android the **Drivers** button opens the selection dialog. It lists the
system driver plus every directory with a native-written
`package.properties` (`id`, `display_name`, `vendor`, `driver_version`,
`min_api`, `soname`). **Import zip** uses SAF; a long press on a row confirms
delete. Selection is written to `filesDir/drivers/selection.txt`:

```text
custom=true
driver_dir=/data/user/0/com.gungnir.recomp/files/drivers/<id>
soname=vulkan.ad07XX.so
hook_lib_dir=/data/app/.../lib/arm64
tmp_lib_dir=/data/user/0/com.gungnir.recomp/cache
```

`custom=false` (or no file) means system. A driver whose `minApi` is newer
than the device is listed but cannot be selected. The native side reads the
file before creating any Vulkan object; a malformed file logs
`driver_selection_invalid` and falls back to system. Selection takes effect on
the next start. The drop folder `getExternalFilesDir()/drivers/` is scanned on
startup and imported through the same native installer; the zip is never
loaded from external storage. On x86_64 builds the dialog reports
`Custom drivers require arm64-v8a` and never imports.

The default path never calls libadrenotools. A driver that fails to open
produces a named error, the app boots on the system driver, and no partial
Vulkan instance is kept.

## Hard constraints

- `useLegacyPackaging = true` in the Android packaging block. The hook
  libraries must be decompressed into `nativeLibraryDir`; Android's default
  APK-direct loading breaks libadrenotools.
- `hookLibDir` must equal `ApplicationInfo.nativeLibraryDir` exactly. A wrong
  path can still return a handle but silently falls back to the original
  driver or reports zero devices.
- `customDriverDir` must be app-private (`getFilesDir()`), never sdcard.
  `dlopen` on a world-writable path is denied by SELinux.
- `tmpLibDir` is required on API < 29 (API 28, Android 9); without a writable
  directory the patched-library path fails when memfd is unavailable.
- Custom drivers are supported on `arm64-v8a` only. `x86_64`, `armeabi-v7a`
  and `armeabi` report **system only** and continue to boot.
- Custom drivers require API 28+ (Android 9+). API 26–27 remain system-only;
  this is a named limitation, not a silent downgrade.
- Hook libraries built from the pinned source: `libmain_hook.so`,
  `libfile_redirect_hook.so`, `libgsl_alloc_hook.so`, `libhook_impl.so`.
  T10 enables only `ADRENOTOOLS_DRIVER_CUSTOM`; BCn toggling, file redirect
  and GPU mapping import are out of scope.

## Fail-closed errors

Every rejection is named and logged. A failed import never becomes selectable;
a failed load never leaves a half-initialized instance.

| Error | Meaning |
|---|---|
| `driver_zip_missing` | selected file or drop-folder entry not readable |
| `driver_zip_unsupported` | not a zip, or an unsupported compression method |
| `driver_zip_slip` | absolute path or `..` component in an entry |
| `driver_entry_duplicate` | two entries share a path |
| `driver_entry_oversize` | entry count or size budget exceeded |
| `driver_entry_name` | empty, overlong or control-character entry name |
| `driver_entry_encrypted` | encrypted zip entry |
| `driver_meta_invalid` | `meta.json` is not valid JSON or has wrong field types |
| `driver_meta_schema` | missing/unknown `schemaVersion` |
| `driver_meta_name` | missing or empty `name` |
| `driver_library_missing` | no usable `.so`, or `libraryName` absent |
| `driver_library_ambiguous` | multiple candidate root libraries |
| `driver_elf_class` | not ELF64 |
| `driver_elf_machine` | not `EM_AARCH64` |
| `driver_elf_type` | not `ET_DYN` |
| `driver_elf_truncated` | headers/dynamic section cut off |
| `driver_elf_dynsym` | empty or unreadable `.dynsym` |
| `driver_abi_unsupported` | device ABI is not `arm64-v8a` |
| `driver_storage_unavailable` | private driver directory cannot be created, or the selected directory is missing/unreadable |
| `driver_hook_library_missing` | `nativeLibraryDir` is missing a libadrenotools hook library |
| `driver_id_invalid` | a driver id is not a 16-character lowercase hash |
| `vulkan_loader_unavailable` | no system Vulkan loader could be opened |
| `driver_load_failed` | libadrenotools returned null for a valid package |

## Troubleshooting

Start with logcat; every failure is named.

| Symptom | What to check |
|---|---|
| Driver is not listed | `driver_import_failed: <reason>` after **Import zip** or a drop-folder startup scan. A malformed zip, wrong ELF machine, missing `libraryName`, or an unsupported package shape never becomes selectable. |
| Driver is listed but cannot be selected | Its `minApi` is newer than the device API. The UI refuses the selection; use the system driver or a package for this Android version. |
| Selection did not apply | Selection is read once at startup. Restart the app. A malformed selection file logs `driver_selection_invalid` and uses the system driver. |
| It fell back to system | `driver_fallback: <reason>` names the cause. `driver_load_failed` means libadrenotools rejected the package; `driver_hook_library_missing` means the APK is missing one of the four hook libraries; `driver_storage_unavailable` means the private driver directory is missing or unreadable; `driver_abi_unsupported` means this ABI is system-only. |
| Custom driver loads but crashes or overheats | A process-level crash cannot be caught. Restart and select the system driver. Driver quality, heat, and game compatibility are the driver author's responsibility. |
| The app crash-loops with a custom selection | There is no automatic safe mode yet. Clear the app's storage or remove `filesDir/drivers/selection.txt` (for example with `adb shell run-as com.gungnir.recomp rm -f files/drivers/selection.txt`), then start the app again. |

Android diagnostics separate the loader stages:
`driver_linkernsbypass_unavailable` (linker namespace bypass failed),
`driver_custom_open_failed` (libadrenotools returned null), and
`driver_custom_symbol_missing` (no `vkGetInstanceProcAddr`). These are logged
once at startup with the final `driver_active` or `driver_fallback` line.

## No-bundling policy

Driver `.zip` files, ICD `.so` files and any extracted driver content are
**user-supplied**. They are never committed, never placed in APK assets or
`jniLibs`, and never redistributed. `repository_check` rejects the root
`drivers/`, `android/app/src/main/assets/drivers/` and source `jniLibs/`
paths by location, and sniffs ZIP and ELF magic so a renamed blob cannot slip
through. The hook libraries built from libadrenotools are project build
output, not drivers, and are covered by the ordinary build-output rules.

## Provenance

| Component | Pin | License | Notes |
|---|---|---|---|
| `bylaws/libadrenotools` | `8fae8ce254dfc1344527e05301e43f37dea2df80`, archive SHA-256 `ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb` | BSD-2-Clause | Fetch-only, built from source for arm64. No local patches. |
| `bylaws/liblinkernsbypass` | `aa3975893d83ef1bc84c321ec60c65fbf1287887`, archive SHA-256 `da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638` | BSD-2-Clause | Transitive submodule, composed into libadrenotools at build time. No local patches. |
| `richgel999/miniz` | `77d0dce8627735138c51770d1799a1ef48f2117d` (3.1.2) | MIT | Vendored byte-identical; ZIP container reader/writer. |
| `nlohmann/json` | `55f93686c01528224f448c19128836e7df245f72` (3.12.0) | MIT | Vendored byte-identical; `meta.json` parser. |
| `zeux/volk` | `aa76c86ae99aa76f7411c981dc885f68a08513b5` | MIT | Vendored byte-identical; Android Vulkan entry-point table. |

Pins and content digests are machine-readable in `dependencies.json`; the
per-subtree records are under `third_party/*/PROVENANCE.md`, and the ledger is
`docs/PROVENANCE.md`. `tools/deps.py verify` checks the pins and
`tools/package_notices.py` checks license coverage. The package content id is
the first 16 hex characters of SHA-256 over the zip bytes, using the adopted
yakumo SHA-256.

## Risks

- Third-party binaries run in the app process with the app's permissions; a
  broken driver can crash or corrupt rendering. Fallback is best-effort: a
  process-level crash cannot be caught.
- Driver quality, thermal behavior and game compatibility are outside this
  project's control. There is no warranty.
- A package can target the wrong Android API or a different Adreno generation;
  it is validated structurally, not functionally.
- Packages are not signed or verified against a trusted publisher. A zip from
  an untrusted source is untrusted native code.
- A bad driver can make startup crash before the Drivers UI is reachable;
  recovering may require clearing app storage or deleting `selection.txt`.
- Non-Adreno GPUs (Mali, PowerVR) are not supported by libadrenotools.
- Importing large zips copies them twice (SAF to staging, then extraction);
  free-space failures are named, not silent.

## Known gaps

- ADPKG packages are not accepted.
- Non-Adreno GPUs (Mali, PowerVR) are unsupported; libadrenotools is
  Qualcomm-specific.
- 32-bit ABIs (`armeabi-v7a`, `armeabi`) and x86_64 custom drivers are not
  supported; only arm64-v8a can import.
- API 26–27 devices are system-only. API 28 uses the `tmpLibDir` path; it was
  not exercised on hardware (HYPOTHESIS).
- Mesa Turnip (Rutabaga-26.10.10) and one Qualcomm v863.1 package have been
  exercised. Other package layouts may still be rejected (HYPOTHESIS).
- No driver auto-update, repository browser, signature verification, or
  publisher trust chain.
- `minApi` is enforced by the Drivers UI; a hand-edited `selection.txt` is not
  re-checked against the device API by the native loader.
- No startup safe mode or crash-loop counter. A driver that crashes the
  process before the UI appears must be removed through app storage or adb.
- File redirect and BCn toggles are not exposed.
- Selecting a driver requires restarting the app; the current session keeps
  its already-created instance.
- Drop-folder imports are scanned at startup only, not while the dialog is
  open.
- Only one Adreno phone and one x86_64 emulator were used for QA; a 30-minute
  thermal/stability soak and a non-Adreno device are not covered.
- The dialog list needed a single-choice list layout; the first build's
  `setMessage` + `setItems` combination hid the rows. Fixed and re-verified.

## Evidence status

| Claim | Label | Evidence |
|---|---|---|
| System driver boot is unaffected on Adreno and the x86_64 emulator | VERIFIED | Three phone cold boots; emulator SwiftShader frame |
| A selected-but-unloadable driver falls back with a named error | VERIFIED | Phone `driver_load_failed`, `driver_hook_library_missing`, `driver_storage_unavailable` |
| A real Turnip package imports, loads, and reports its version | VERIFIED | Rutabaga-26.10.10, three phone cold boots, Mesa 26.3.0-devel |
| The Qualcomm v863.1 package loads | VERIFIED | One phone boot, Vulkan 1.4.295 |
| Malformed zip, wrong-ABI ELF, missing hooks and missing private dir fail closed | VERIFIED | Device imports and loader fallbacks |
| Driver blobs are rejected by repository path and magic | VERIFIED | Red-first probes and work-tree gate |
| API 28 `tmpLibDir` path | HYPOTHESIS | Not exercised on API 28 hardware |
| Other Turnip/Qualcomm package layouts | HYPOTHESIS | Only two packages exercised |
| Non-Adreno, 32-bit and x86_64 custom drivers | UNSUPPORTED | Architecture limits, not tested |
| Crash-loop recovery / safe mode | PLANNED | Named gap above |
| 30-minute thermal/stability soak | PLANNED | Not part of T10 QA |

## Mutation evidence

Each mutation was applied locally, the loader suite was run, and the named
check failed for the intended reason. The mutations were reverted.

| Mutation | Observed failure |
|---|---|
| The system path calls the custom opener | `FAIL: system path never calls the custom opener` |
| The ABI guard is removed | `FAIL: bad_function_call` (empty custom opener invoked) |
| The fallback retains the failed custom handle instead of closing it | `FAIL: failed custom handle is closed` |

## References

- `gh search code "adrenotools_open_libvulkan" --extension cpp`: PPSSPP
  `Common/GPU/Vulkan/VulkanLoader.cpp`, Dolphin `VulkanLoader.cpp`, Vita3K
  `vita3k/util/src/android_driver.cpp`, Flycast `core/rend/vulkan/adreno.cpp`,
  Azahar `src/android/app/src/main/jni/native.cpp`.
- `gh search code "meta.json libraryName" --extension cpp`: PPSSPP
  `UI/DriverManagerScreen.cpp` (example schema and validation).
- `gh search code "useLegacyPackaging" --extension kts`: Android packaging
  usage across emulator/front-end projects.
- `gh search issues "adrenotools driver zip"` and `gh search issues "Turnip
  driver fails to load"`: reported API 28/29 and packaging failures.
- libadrenotools `include/adrenotools/driver.h` and README (BSD-2-Clause);
  `liblinkernsbypass` submodule gitlink at the pinned commit.
- `gh search code "custom GPU drivers" --extension md`: NocturneRecomp's
  custom-GPU-driver guide (package shape, private storage, selection/restart
  behavior) and libadrenotools' README (Android 9+, arm64, developer-only
  library).
- `gh search code "libadrenotools" --extension cpp`: Vita3K, Flycast,
  aps3e and other recompilation projects calling
  `adrenotools_open_libvulkan`.
- `gh search issues "adrenotools documentation"`: no additional loader
  regression; issue #25 covers source-archive/submodule composition only.
- `gh search code "driverPackage" "vulkan" --extension cpp`: Android
  `GraphicsEnv`/`GpuService` driver-package naming, not an app loader.
- PPSSPP driver-manager behavior was used as a diff oracle for the package
  list and restart semantics; no PPSSPP code is linked or copied.
- Device evidence and the trailing-slash finding:
  `docs/qa/2026-10-31-t10e-driver-matrix.md`.

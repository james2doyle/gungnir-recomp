# ANDROID_DRIVERS — custom Adreno Vulkan drivers

Status: T10a contract and loader decision. The inspector, loader, UI and QA
land in T10b–T10e. No driver package or binary is bundled with this project.

Gungnir can render through the device's system Vulkan driver (the default) or
through a user-supplied Mesa Turnip-style ICD package. This file defines what
a package is, where it may live, how it is validated, and how failures behave.

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
| `minApi` | int | no | lowest Android API; default `0`; greater than the device API is refused |
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
ADPKG is not accepted in T10.

### Archive and ELF limits (provisional, T10b enforces)

- compressed archive at most 512 MiB, at most 4096 entries, at most 256 MiB
  per entry, at most 1 GiB uncompressed total;
- no absolute paths, no `..` components, no duplicate names, no encrypted
  entries, no unsupported compression methods;
- the ICD must be ELF64, little-endian, `ET_DYN`, `EM_AARCH64`, with a
  non-empty `.dynsym`; 32-bit, `EM_X86_64`, `ET_REL`/`ET_EXEC`, and truncated
  files are rejected before extraction.

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
   `VK_KHR_driver_properties` (`driverName` / `driverInfo`). The UI shows the
   same values.

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
| `driver_zip_unsupported` | not a zip, encrypted archive, or unsupported method |
| `driver_zip_slip` | absolute path or `..` component in an entry |
| `driver_entry_duplicate` | two entries share a path |
| `driver_entry_oversize` | entry count or size budget exceeded |
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
| `driver_min_api` | package requires a newer Android API |
| `driver_abi_unsupported` | device ABI is not `arm64-v8a` |
| `driver_storage_unavailable` | private driver directory cannot be created |
| `driver_load_failed` | libadrenotools returned null for a valid package |

## No-bundling policy

Driver `.zip` files, ICD `.so` files and any extracted driver content are
**user-supplied**. They are never committed, never placed in APK assets or
`jniLibs`, and never redistributed. `repository_check` (T10e) rejects them
under the repository, Android asset and native-library paths. The hook
libraries built from libadrenotools are project build output, not drivers, and
are covered by the ordinary build-output rules.

## Risks

- Third-party binaries run in the app process with the app's permissions; a
  broken driver can crash or corrupt rendering. Fallback is best-effort: a
  process-level crash cannot be caught.
- Driver quality, thermal behavior and game compatibility are outside this
  project's control. There is no warranty.
- A package can target the wrong Android API or a different Adreno generation;
  it is validated structurally, not functionally.
- Importing large zips copies them twice (SAF to staging, then extraction);
  free-space failures are named, not silent.

## Known gaps

- ADPKG packages are not accepted.
- 32-bit and x86_64 custom drivers are not supported.
- API 26–27 devices are system-only.
- Only Mesa Turnip-style packages have been exercised; Qualcomm packages with
  a different layout may be rejected.
- No driver auto-update, repository browser or signature verification.
- File redirect and BCn toggles are not exposed.

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

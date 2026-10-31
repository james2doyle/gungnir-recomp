# libadrenotools provenance

- Upstream: https://github.com/bylaws/libadrenotools
- Pinned commit: `8fae8ce254dfc1344527e05301e43f37dea2df80`
- Archive: `https://github.com/bylaws/libadrenotools/archive/8fae8ce254dfc1344527e05301e43f37dea2df80.tar.gz`
- Archive SHA-256: `ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb`
- License: BSD-2-Clause (`LICENSE` in this directory, copied verbatim)
- Disposition: **fetch-only**, built from source on the machine that builds the
  app. No upstream source, object, or binary is committed here; only the
  license and this note are tracked.

## Why it is adopted

The alternative is a clean-room Vulkan ICD loader. That is exactly what
`AGENTS.md` hard limit #0 forbids without explicit user sign-off. This project
adopts libadrenotools instead: it is BSD-2-Clause (GPL-3.0-compatible), mature,
and already used by PPSSPP, Dolphin, Vita3K, Flycast, Strato and Azahar for the
same job.

## What the Android build uses

- `include/adrenotools/driver.h`, `include/adrenotools/priv.h`
- `src/driver.cpp`, `src/bcenabler.cpp`
- `src/hook/` shared hooks: `libmain_hook.so`, `libfile_redirect_hook.so`,
  `libgsl_alloc_hook.so`, `libhook_impl.so`

The build enables only `ADRENOTOOLS_DRIVER_CUSTOM`. `BCn` enabling, file
redirect and GPU mapping import remain off in T10; they would each need their
own contract and QA.

## Submodule composition

`lib/linkernsbypass` is a git submodule upstream and is **not** included in the
GitHub commit archive. It is pinned as a separate `dependencies.json` entry
(`bylaws/liblinkernsbypass` at `aa3975893d83ef1bc84c321ec60c65fbf1287887`)
and materialized into `lib/linkernsbypass` before configuring libadrenotools.
Both archives are verified before extraction. The materialized tree lives under
the gitignored `build/android-deps/` tree.

## Local patches

None. The fetched tree is built unmodified. Caller-side shims (selection,
staging, named errors) live under `runtime/platform/adreno/`, outside the
upstream tree.

## Hard constraints carried into our contract

- `useLegacyPackaging = true`; `hookLibDir` must equal
  `ApplicationInfo.nativeLibraryDir`.
- `customDriverDir` must be app-private (`getFilesDir()`), never sdcard.
- `tmpLibDir` is required on API < 29 (Android 9, API 28) because memfd may be
  unavailable.
- ARM64 only; upstream rejects other ABIs at configure time.

## References checked

- `gh search code "adrenotools_open_libvulkan" --extension cpp` — PPSSPP
  `Common/GPU/Vulkan/VulkanLoader.cpp`, Dolphin `VulkanLoader.cpp`, Vita3K
  `android_driver.cpp`, Flycast `adreno.cpp`, Azahar `native.cpp`.
- `gh search code "meta.json libraryName" --extension cpp` — PPSSPP
  `UI/DriverManagerScreen.cpp`.
- `gh search issues "adrenotools driver zip"` and `gh search issues "Turnip
  driver fails to load"` — Android 9/10 loading reports; no contrary contract
  found.

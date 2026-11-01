# TESTING — oracle hierarchy and fixture rules (T8 state)

Policy: plan §8.1 (red-first) and §8.2 (oracle hierarchy). This file covers
the T4 core harness, the T5 profile gates, the T6 HLE fixtures, the T7
GE/Vulkan gates and the T8 texture gates; T9 adds the Android/device gate.

## Oracle hierarchy

1. **Hand-computed expectations** — values derived by hand from the instruction
   encoding and written into the test driver (`tests/test_core_fixtures.py`,
   `HAND_COMPUTED`). Never generated from the implementation.
2. **The in-repo interpreter** — the semantic reference tier. Every fixture
   runs twice: interpreter tier vs AOT tier; the function-boundary traces and
   final architectural digests must be **byte-identical** (TC-4.1).
3. **Synthetic golden traces** — the fixture trace files themselves; any
   semantic mutation (opcode semantics, dispatch, memory map) must break
   equality.
4. **PPSSPP headless diff** — optional later bring-up oracle, test-only,
   never linked (plan §7F.7).
5. **On-device manual QA** — mandatory for device-facing milestones (plan §8.3).

## Red-first loop

- Bug fixes start from a failing fixture in `tests/core/fixtures/` whose stop
  reason or digest pins the bug (examples already recorded: ADDI decoding,
  scratchpad access, the VFPU forms in `vfpu_boot_gap.mips`).
- A fixture is added red, observed failing for the named reason, then fixed;
  the observation is recorded in the task/commit notes.
- Mutation checks: for codegen/HLE semantic changes, break one semantic and
  confirm the trace suite dies for that reason (TC-5.3 formalizes this for the
  Gungnir profile).

## Fixture format (`tests/core/fixtures/*.mips`)

```
# name: <fixture>
# sentinel: <absolute PC the fixture returns to; harness registers it>
# sp: <initial $sp>
# mem: <addr> = <hex bytes>      # initial guest memory (repeatable)
# digest: <start>:<end>          # memory windows included in state digests
<0xWORD>  # comment
...
```

Fixtures are linked as plain executables at `0x08804000` (one absolute
PT_LOAD), so guest literals are real runtime addresses. `assemble.py` emits the
ELF + functions CSV + metadata into `build/` — no binary fixture is ever
tracked (repository_check would reject ELF blobs; `.gitignore` covers build/).

## Trace format

Per dispatch at a declared trace PC (and at the sentinel):

```
start <pc> <sha256(state)>
pre   <pc> <sha256(state)>
end   <stopped|budget> <pc> <sha256(state)>
final-regs r0=… r31=… hi=… lo=…
final-mem <addr> <hex bytes…>
reason <verbatim runtime stop reason>
```

`state` covers gpr[32], hi/lo, fpr bits, vfpu bits, vfpu_ctrl, and the
fixture's declared memory windows. Stops exit 4 (loud); clean fixture
completion exits 0; an exhausted dispatch budget exits 3 — never silent
success.

## Profile gates (T5)

`tests/test_profile.py` adds four gates on the real local module. The suite
skips when the private disc is absent; it never passes silently.

- TC-5.1 `TestAnalysisManifest`: accounting identity
  (`analyzed + unreached == executable words`), census bounds asserted by the
  tool, required discovery sources present, 215/215 import stubs mapped,
  deterministic manifest bytes.
- TC-5.2 `TestCodegen`: every executable word is a registered entry, import
  wrappers match the inventory, repeated codegen is byte-identical and rewrites
  zero units, and the whole generated corpus compiles.
- TC-5.3 `TestMutationGate`: changing one generated fixture semantic
  (`+1` -> `+2`) must make the interpreter/AOT traces diverge with a changed
  guest register. A build error does not count; the mutation must kill the
  gate for the right reason.
- TC-5.4 `TestDispatchCoverage`: a bounded real-module run classifies every
  outer dispatch as translated or interpreter fallback; the negative control
  drops the entry unit and requires a nonzero, listed fallback count.

## HLE gates (T6)

- `tests/test_nids.py`: every Gungnir main-module import resolves to a name;
  CSV digest is pinned; PSPSDK provenance/license are recorded.
- `tests/test_hle.py`: cross-library symbol alias binds and invokes; an
  unbound import stops with `Missing HLE import … (NID_0x…)`, never a silent
  zero. `tests/hle/kernel_tests.cpp` covers memory partitions, thread
  lifecycle/preemption/delay, semaphores, event flags, blocking waits, VTimer
  interrupts, VPLs, lightweight mutexes, ISO disc + memory-stick IO, and async
  open/read/lseek/close staging through Wait/Poll/GetAsyncStat (busy, failure,
  no-async, closed-descriptor and bad-whence errors), and the sceUtility
  savedata SIZES state machine (filled usage blocks, missing-save NO_DATA, bad
  param size and path traversal).
  `tests/hle/media_tests.cpp` covers display mode/flip/vblank/multi-wait/
  frame-rate, controller sampling and vblank-blocking reads, GE callback and
  list completion, EDRAM, and the RTC tick. `tests/hle/module_tests.cpp`
  covers module lookup, stop/unload errors, and a corpus install → unload →
  different-corpus reload cycle with a stable function count.
- `tests/test_boot.py` (TC-6.2): builds the real-module boot runner against
  the T5 corpus and requires module init to reach a non-zero-width
  `sceDisplaySetFrameBuf`, with every executed import bound and no missing or
  silent NID. Two `--no-stop-at-flip` runs must produce identical deterministic
  fields (dispatches, virtual time, vblanks, flips, import lists, stop
  reason/PC) — TC-6.4. The determinism runs are bounded by 200,000 dispatches,
  not wall time, because the title now enters its main loop.

## Commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 -m unittest tests.test_core_fixtures -v     # TC-4.1..TC-4.4
python3 -m unittest tests.test_profile -v           # TC-5.1..TC-5.4
python3 -m unittest tests.test_nids tests.test_hle -v  # T6a/T6b registry + kernel/HLE
python3 -m unittest tests.test_android_build -v     # TC-9.1 Android build skeleton
python3 -m unittest tests.test_android_app -v       # SDL3 pin/fetch + shell scripts
python3 -m unittest tests.test_game_data -v         # TC-9.3 game-data identity gate
python3 -m unittest tests.test_affinity -v          # T9e CPU affinity/priority
python3 -m unittest tests.test_android_suite -v     # consolidated T9 gate
python3 -m unittest tests.test_adreno_docs -v       # T10a driver contract + pins
python3 -m unittest tests.test_driver_package -v    # T10b driver zip inspector
python3 -m unittest tests.test_vulkan_loader -v     # T10c loader selection/fallback
python3 -m unittest tests.test_driver_store -v      # T10d driver inventory/selection
python3 -m unittest tests.test_adreno_suite -v      # consolidated T10 gate
python3 -m unittest tests.test_input -v            # T11a input core + sceCtrl mapping
python3 -m unittest tests.test_touch_overlay -v    # T11b layout/pointer/edit/JSON
python3 -m unittest tests.test_touch_haptics -v    # T11c touch-haptics policy
python3 -m unittest tests.test_gamepad -v          # T11d profiles/state/remap
python3 -m unittest tests.test_sdl_gamepad -v      # T11d virtual-gamepad adapter
python3 -m unittest tests.test_input_suite -v      # consolidated T11 gate + docs
python3 -m unittest tests.test_audio_mixer -v      # T12a mixer ring/volumes/underrun
python3 -m unittest tests.test_sdl_audio -v        # T12a SDL3 tone smoke
python3 -m unittest tests.test_savedata -v         # T12b store/SFO/save-load + PPSSPP saves
python3 -m unittest tests.test_dar -v              # T12b DAR list/extract/repack
python3 -m unittest tests.test_docs_building -v    # T13c BUILDING.md contract
python3 -m unittest discover -s tests -p 'test_*.py' -v   # full battery

`tests.test_docs_building` checks that [`BUILDING.md`](BUILDING.md) exists,
that every script and build flag it names is real, that relative links
resolve, and that it contains no machine paths, dump hashes, screenshots or
performance claims the tree does not support.

Consolidated gates (`test_android_suite`, `test_adreno_suite`,
`test_input_suite`, `test_texture_suite`, `test_ge_suite`) still run their
child modules when invoked on their own. Under full discovery they skip the
children that the discovery run already executed, so a full battery does not
build the APK or the GE/texture binaries twice. The skip reason says which
modules are covered.
```

Golden-file tests (post-processing pins) and PPSSPP diffs live in T7; Android
device QA starts with T9 and the full milestone campaign is T13.

## GE renderer gates (T7)

`tests/test_ge_suite.py` is the consolidated TC-7.1–TC-7.4 gate: it runs the
display-list, vertex, texture, software-rasterizer, Vulkan and post fixtures
and checks the renderer/shader documents. Synthetic fixtures only.

- TC-7.1: commands, vertex formats, textures, triangles/strips/fans/sprites,
  blend, scissor, depth, alpha test and stencil-lite.
- TC-7.2: software vs Vulkan readback, tolerance 1 LSB per 8-bit channel.
- TC-7.3: pinned post-preset hashes (default, integer, scanline, LCD-grid,
  FSR-lite).
- TC-7.4: texture-range and CLUT re-upload invalidation.

Mutation evidence and the remaining named gaps (full GE state machine -> draw
path wiring, DXT textures, Vulkan post passes, validation-layer availability)
are recorded in `docs/RENDERER.md`.

## Texture container gates (T8)

`tests/test_tpl.py` builds and runs `tex_tpl_tests`: synthetic TPL containers
(no game bytes) covering CLUT4 alpha ramps, RGB565 palettes, CLUT8 palettes,
odd widths, multi-texture containers, counted auxiliary resources and named
malformed-input failures. Writer tests assert byte-identical decode -> encode,
single-byte texel/palette edits, and rejected inconsistent models. The local
ASan/UBSan scan round-tripped 1,304 containers byte-identically;
`docs/formats/TPL.md` records the layout and the one local named gap
(`debugFont.tpl`, pixel format `0x0108`).

`tests/test_tex_dump.py` runs `tex_dump_tests` (built when libpng is
available): content-key stability/sensitivity and sampler independence, RGBA8
PNG round-trip, `textures.ini` creation/de-duplication, CLUT-change dumps,
existing-file collision and corrupt-file rejection. `docs/TEXTURE_REPLACEMENT.md`
records the dump layout and key rules.

`tests/test_tex_replace.py` runs `tex_replace_tests`: 2x/4x replacements
rendered through the software rasterizer, native fallback, replacement state
derivation, unmatched/corrupt scan reporting, cache/clear reload, dump vs
replacement isolation and invalid inputs.

`tests/test_font_repack.py` covers the font-atlas tool: identity gate,
2x/4x scaling, ink-width regeneration, a collision-free test string and
refusals that leave no output.

`tests/test_texture_suite.py` is the consolidated TC-8.1–TC-8.4 gate over the
four groups above. It also checks the TPL/DAR/replacement documents, the
mutation ledger and the named-gap list. `docs/TEXTURE_REPLACEMENT.md` is the
user-facing dump/replacement/font guide.

## Android gates (T9)

`tests/test_android_build.py` covers TC-9.1: SHA-pinned wrapper,
property-only machine paths, `.gitignore` coverage, named failure when the
SDK/NDK pin is missing, a relocated-tree build, both ABI `libmain.so`
libraries, statically linked SDL3, the packaged SDL Java layer and an exported
`SDL_main`.

`tests/test_android_app.py` checks the SDL3 `release-3.4.18` pin against
`cmake/Deps.cmake`, fetch reuse from a verified archive, tampered-archive
rejection and the deploy/lifecycle scripts.

`tests/test_ge_suite.py` exercises the windowed swapchain path, including
`VulkanBackend::recreate_surface` (destroy/rebuild the VkSurfaceKHR and
swapchain) before rendering again. On device, `scripts/android/lifecycle.sh`
backgrounds and foregrounds the app three times, requires
`lifecycle_resumed` after each cycle, verifies that frames remain landscape
even with `user_rotation 0`, and checks audio pause/resume when the tone probe
is available. The phone run passed with 3063x1335 frames.

`tests/test_game_data.py` runs `game_data_tests`: synthetic `game/*.iso`
discovery (case-insensitive, sorted), size and CRC32 gate rejection, corrupt
candidate skip with a valid fallback, missing-data naming, and runtime
asset-stamp matching. The device check additionally pushed the real disc ISO
to `getExternalFilesDir()/game/` and exercised the SAF picker; both paths
reported `game_data_ok` with size 262766592 and CRC32 `183F3EDC`.

`tests/test_affinity.py` runs `affinity_tests`: CPU-list parsing, highest-
allowed-CPU selection, injected affinity/priority success and denial, and
missing status/list named errors. `tests/test_android_build.py` also checks
the generated CMake compile commands: the runtime target carries `-O2` and
`-fno-lto` even in debug, and the `-Pgungnir.o1Sources` named unit carries a
later `-O1`. Device evidence: the phone and emulator both logged
`affinity_applied` and `priority_applied`; denial is logged, never fatal.

`tests/test_android_suite.py` is the consolidated T9 gate. It runs the build,
SDL3, game-data and affinity modules, checks `docs/ANDROID.md` and the T9 QA
report, and verifies that screenshots land under the gitignored `build/qa/`
tree. Device and emulator evidence is in
`docs/qa/2026-10-30-t9-android-shell.md`; tests never substitute for it.

`tests/test_adreno_docs.py` is the T10a contract gate. It asserts the
libadrenotools and liblinkernsbypass fetch pins (commit, archive SHA-256,
license, path), their license/provenance files, and the driver contract in
`docs/ANDROID_DRIVERS.md`: the adoption decision, both package shapes,
`meta.json` fields, private-storage and packaging constraints, the named-error
table, and the no-bundling policy. No driver archive is downloaded or
committed by the test.

`tests/test_driver_package.py` runs `driver_package_tests`, which synthesizes
zips with miniz and synthetic ELF64 headers (no driver binary). It covers
stored and deflated entries, Turnip `meta.json` and bare `libvulkan.so`
packages, idempotent install/remove, zip-slip (including a patched absolute
name), duplicates, overlong names, entry/archive budgets, encrypted and
unsupported-compression entries, schema/name/library/minApi failures, wrong
ELF class/machine/type, truncated ELF, empty `.dynsym`, drop-folder scanning,
and atomic failure (no staging directory left behind). Mutation checks:
disabling the encryption guard returns a different named error for the
encrypted fixture, and disabling the path guard accepts the zip-slip fixture;
both make the suite die for the intended case.

`tests/test_vulkan_loader.py` runs `loader_tests` with injected openers: the
system path never calls the custom opener and tries the system names in
order; a custom selection forwards its dirs/soname and succeeds; the driver
directory is normalized with a trailing slash because libadrenotools
concatenates it with the soname; custom failure (missing ABI support,
missing/unreadable driver directory, missing hook library, null handle,
missing entry point, invalid selection) falls back to system with the named
error retained; total failure is `vulkan_loader_unavailable`; shutdown closes
the active handle once.
`tests/ge/vulkan_tests.cpp` also asserts `driver_info()` reports a loaded
driver, a non-empty name and an API version after initialization. The Android
system path was exercised on the x86_64 emulator, which logged
`driver_active=system name=gfxstream (SwiftShader driver)` before the first
frame. The arm64 phone logged
`driver_active=system name=Qualcomm Technologies Inc. Adreno Vulkan Driver`
and `first frame 3063x1335`; a real custom driver remains T10e.

`tests/test_driver_store.py` compiles `DriverStore.java` and its plain-JVM
harness, then checks the installed-driver listing from `package.properties`,
the default system selection, selection-file contents (driver dir, soname,
nativeLibraryDir hooks and tmp dir), system-selection deletion, invalid-id
refusal, recursive delete and drop-folder discovery. The emulator UI was
exercised over adb: the Drivers button opened the dialog, which reported
`Custom drivers require arm64-v8a. This device uses the system driver.`,
while logcat still showed `driver_active=system` and the first frame. On the
arm64 phone the dialog listed the system driver plus a synthetic imported
package; selecting it produced `driver_fallback: driver_load_failed` and the
system frame, SAF import reported `driver_import_ok`, and delete restored
system. The arm64 phone then loaded a real Rutabaga/Turnip package and
v863.1; the QA matrix is in `docs/qa/2026-10-31-t10e-driver-matrix.md`.

`tests/test_repository_check.py` adds T10e probes for driver payloads:
ZIP magic catches a renamed archive, and the root `drivers/`, asset-side
`drivers/` and source `jniLibs/` locations fail even when force-added. Device
evidence for the fail-closed matrix is in
`docs/qa/2026-10-31-t10e-driver-matrix.md`: three phone cold boots on the
system driver, named fallback for `driver_load_failed`,
`driver_hook_library_missing` and `driver_storage_unavailable`, malformed and
wrong-ABI imports, the x86_64 `driver_abi_unsupported` path, and three cold
boots loading a real Rutabaga/Turnip package. The device run found and fixed
the missing trailing slash in the driver directory passed to
libadrenotools.

`tests/test_adreno_suite.py` is the consolidated T10f gate. It runs the
driver-contract, package-inspector, loader, inventory and repository modules,
then checks the finished `docs/ANDROID_DRIVERS.md`: package sources and risks,
both schemas, UI/private-storage rules, fallback diagnostics, provenance pins,
named gaps with VERIFIED/HYPOTHESIS/UNSUPPORTED/PLANNED labels, mutation
evidence and the T10e QA report. It also asserts that no driver archive or
native library is tracked. The three mutation checks (system path calling the
custom opener, ABI guard removed, fallback retaining the failed handle) were
run locally and recorded in the doc; the mutations were reverted.

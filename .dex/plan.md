# Gungnir Recomp — Master Plan

**Target:** Gungnir (USA) — *Gungnir: Inferno of the Demon Lance and the War of Heroes*, ULUS-10592
**Goal:** Static recompilation of the game's Allegrex code to native C++, running on Android — from the user's own `game.chd` to real gameplay on an adb-connected device.
**Current milestone (locked):** M4 QA close-out (gameplay into battles verified on device; dated QA reports pending) with battle-scene performance work feeding M5.
**Plan date:** 2026-10-30; amended 2026-11-03 (T16 research corrected: 16:9 is a no-op for this title, T16b must land first). Source: 14-project discovery survey (see §3).

---

## 0. Hard constraints (non-negotiable)

1. **No proprietary content, code, or work is ever committed.** No game ISO/CHD/ELF/PRX, no decrypted modules, no game assets (DAR contents, TPL textures, VAG audio, script `.bin`), no game text (original or translated), no game screenshots, no Sony firmware, no PGF fonts, no KIRK/DRM keys, no Adreno driver binaries, no generated recompilation output (which is a derivative of the copyrighted executable).
2. The repo contains only: original tooling and runtime we author, third-party code with licenses preserved and attributed, synthetic fixtures of our own authorship, documentation, and scripts that operate on **user-supplied local files**.
3. Decryption of `~PSP` modules runs **out-of-process** using user-supplied tools (pspdecrypt / PPSSPP devtools). We ship no decryption implementation and no keys. Same for Adreno driver `.zip`s: user-supplied, extracted to app-private storage, never bundled or committed.
4. Enforcement is structural, not aspirational: `.gitignore` with rationale, `tools/repository_check.py` (extension + magic-byte + blob-size + ignore-rule audit) in CI, secret scanning, and fail-closed CI (`exit 77` skips) for tests that need private game input.
5. Game-derived facts that are acceptable to publish: disc identity (size 262,766,592 B, CRC32 `183F3EDC`, redump-recorded), format field layouts, NID values, guest addresses and record sizes (RE facts). Binaries, hashes of binaries pinned as "the expected dump", and content derived from them stay local.
6. **No clean-room reimplementations.** If work reduces to implementing a format, codec, or protocol that existing tools or libraries already implement, **stop and ask before writing anything**: use the existing tool out-of-process, or adopt an audited open-source implementation with provenance. Clean-room reimplementation is not sanctioned without explicit user sign-off (AGENTS.md hard limit #0).

## 1. Decisions locked (2026-10-30)

| Decision | Choice |
|---|---|
| Foundation | Adopt an **MIT-licensed PSPRecomp-framework core** (jessicanataliagta/PSPRecomp lineage; TeamGDB/Yakumo framework subset is the same lineage, more mature). We write: Gungnir profile, Android shell, Vulkan renderer, driver loader, texture/shader systems. |
| Project license | **GPL-3.0-or-later** (max reuse freedom: MIT + BSD + LGPL-dynamic + GPL-2.0-or-later + GPL-3.0 sources; rejects CC BY-NC-SA and unattributed code) |
| Android baseline | **minSdk 26, Vulkan 1.1 required**, AAudio available; ABIs `arm64-v8a` + `x86_64` (emulator) |
| Renderer | **Vulkan-only** (no GL ES fallback); software rasterizer kept as parity oracle; post-processing chain (upscale + LCD/scanline shaders) after GE compositing |
| Renderer build-vs-adopt | Keep the validated T5/T6 stack. Use **psprecore** (MIT) as the primary behavioral reference and targeted port source for T7a–T7d (GE walker, vertex pipeline, software rasterizer, texture sampler) and as a cross-check for T7e/T7f; use **ff7ccrecomp** for Android packaging patterns. A wholesale fork is reconsidered only if `nx-foundation` gains a compatible license **and** a 3–5 day spike boots Gungnir to the T6 gate (200k dispatches, no missing import, main-loop threads). Until then, no vendoring of psprecore or its submodules |
| Audio | AAudio (Android) / SDL audio (desktop); ATRAC3(+) via LGPL `at3_standalone`-class decoder with notices |
| Game data on device | SAF document-tree picker **or** `adb push` to `getExternalFilesDir()`. **No** `MANAGE_EXTERNAL_STORAGE` |
| Adreno drivers | User-supplied `.zip` ICDs (Mesa Turnip / Qualcomm-style) loaded via custom Vulkan loader; per-driver selection; fail-closed fallback to system driver |
| Input | Touch overlay (editable layout) + SDL3 gamepad with per-device profiles (Ayn/Ayaneo-class handhelds) + keyboard/mouse (desktop dev); rumble/haptics via SDL gamepad rumble + Android Vibrator fallback |
| Textures | Hash-keyed dump→replace pipeline (PPSSPP `textures.ini` semantics) + high-res font-atlas re-rasterization for dialog text (Gungnir stores text in TPL atlases and baked into TPL images) |
| Disc input | **Intake accepts ISO only.** `game.chd` is converted once, user-side — e.g. [CHDStudio](https://github.com/purelogiccode/CHDStudio) (GUI, recommended) or `chdman extractcd` (MAME CLI). Our pipeline never parses CHD. Converter correctness is *verified, not trusted* — the identity gate (size + CRC32) validates whatever produced the ISO. |
| Code reuse | **Adopt, don't reimplement.** Clean-room implementations of existing formats/codecs/protocols are **not sanctioned** without explicit user sign-off (AGENTS.md #0): use existing tools out-of-process (CHDStudio/chdman) or adopt audited open-source code with provenance (Yakumo MIT subset → `third_party/yakumo/`). Applies to everything upcoming too (e.g. PGF parsing, ATRAC3, image codecs, JSON) — check the reference projects and library landscape first, always. |
| Testing | **Red-first (TDD where relevant):** the failing test lands before the implementation, and is confirmed red for the expected reason. Regression fixes start from a failing reproduction. No task closes with untested behavior. |
| Device validation | **Mandatory manual QA on an adb device** before any milestone touching device behavior can close — structured, recorded QA (build ID, device, steps, expected/actual, sign-off), never "CI is green" alone |

## 2. Target facts — Gungnir (USA)

Facts from the discovery survey (kreycai/gungnir-ptbr format docs, redump, PPSSPP logs). These shape the whole plan:

- **Disc:** UMD ISO 9660, 262,766,592 B, CRC32 `183F3EDC` (redump 29734). The local `game.chd` is an LZMA CHD of this disc (header verified: `MComprHD` v5, `lzma`). JP is ULJM-05779 (v1.01) — different offsets everywhere; this plan targets **USA only**.
- **`PSP_GAME/SYSDIR/EBOOT.BIN`:** 3,336,096 B at ISO offset `0x110000`, `~PSP`-encrypted (magic `0x5053507E`); `BOOT.BIN` sibling is the plaintext form on some discs. Decrypted form: PRX ELF `e_type 0xFFA0`, segment 0 at file offset `0xC0` with `vaddr = file_offset − 0xC0`, relocation section type `0x70000020` (entries 8 B: `r_offset:u32, r_info:u32`, types **2 = R_MIPS_32, 5 = HI16, 6 = LO16**). ~3,552 strings live in the EBOOT (vaddr `0x19F000–0x1D8000` region).
- **USRDIR archives (DAR format):** `ini.dar` (61 files incl. font atlases `font11pt.tpl`/`font12pt.tpl`), `voice.dar` (voice audio), `data.dar` (~44 MB, plaintext, 1,295 `.tpl` textures + ~13,998 strings + script `.bin`s), plus `INSDIR/DATA.DAR`. DAR header = 4×u32 LE `[nfiles, data_start, names_off, table_off]`; entries = 16 B `[name_off, stored_size, decomp_size, data_off]`; zlib per entry when `decomp_size > stored_size`; **`stored_size == 0` means stored uncompressed** (real size in `decomp_size`).
- **Text:** fullwidth Shift-JIS (cp932), NUL-terminated; engine break commands `5C 6E` in DAR data, `0x3B` in EBOOT. Script `.bin` files (`event/`, `map/`, `worldmap/`, `setup*/`) = `[u32 type, u32 n, u32 table_off=12]` + n absolute u32 offsets + bytecode with absolute-address jumps — **files must not change size** or every pointer/jump must be remapped.
- **Fonts/textures:** TPL containers (0x30 B header; per-texture `H,W,BW,bpp,data_off`), 4bpp PSP-swizzled (16×8 blocks), index-as-alpha, and 8bpp+CLUT. Dialog glyph advance = ink width (width table). UI text is also **baked into images** (actname/tutorial/location pages) — text upscaling has two fronts: font atlas re-rasterization + image texture replacement.
- **Engine/runtime expectations (PPSSPP logs):** calls `sceKernelLoadModule` (PRX overlay tiers needed), "render to area containing texture" (feedback path in GE), one audio-decode quirk. PPSSPP rates it Perfect→Playable — our behavioral oracle.
- **DAR size fields in ISO 9660 directory records are both-endian** (LE + BE 4 bytes later) — any repack must patch both (we don't repack the ISO for recomp, but the asset pipeline touches DARs inside `build/`).

## 3. Reference projects — what each gives us

All listed repos were surveyed 2026-10-30 (see conversation research threads). Use as **patterns and references**; for new HLE import families, start with `gh search code "<library>::<function>" --extension cpp` (function and library names; hex NIDs do not index reliably): it returns concrete HLE snippets and the projects they belong to. Follow with `gh search issues "<function>"` for semantic discussions and regressions, and read the linked threads and code before implementing. copy code only where the license allows (we are GPL-3.0-or-later, so MIT/BSD/LGPL/GPL-2.0+ are all adoptable with attribution; CC BY-NC-SA and unattributed code are not).

| Project | License | What we take from it |
|---|---|---|
| **Jstar269/nakagawa-recomp** | GPL-3.0 | **The Android deployment template**: `android/` Gradle+CMake skeleton (AGP/NDK pins, `-P` properties for machine paths, `useLegacyPackaging`, deps fetch with pin+digest), `nk_add_title_runtime()` CMake shape, two-process Activity model, env contract, `.gitignore` with rationale comments, AGENTS.md hard-limits culture, CI fail-closed (`exit 77`) + synthetic-fixture-only policy, generated notices from one JSON source of truth |
| **AdamFull/psprecore** | MIT | **Closest complete peer**: static recompiler + runtime + GE/Vulkan/software renderers + HLE kernel/audio/savedata/MPEG + Linux/Android frontend, no game data committed. Use as a behavioral/reference implementation for T5/T6/T7/T9/T12: GE walker (`runtime/src/ge.cpp`), vertex pipeline, software rasterizer, texture sampler, Vulkan context and shaders. ⚠️ Its `nx-foundation` submodule has **no LICENSE**; never vendor that subtree, and replace `nx::` types when adapting MIT psprecore code. Its shaders are Slang (`slangc`), not our GLSL/SPIR-V path |
| **AdamFull/ff7ccrecomp** | MIT | **Android/Vulkan consumer of psprecore** (Crisis Core): real Gradle app, SDL3 Java from the build tree, `minSdk 29` for Vulkan 1.1, NDK 29/compileSdk 36, `useLegacyPackaging` for libadrenotools, texture collection/replacement, recordings/replays, decompilation overrides. Use for T9/T10/T8/T13 patterns, not game code/assets. Generated game output is gitignored |
| **zPSP-Dev/Zig-PSP** | MIT (LICENSE; PSPSDK reference note) | **GE command/state reference for T7**: `src/ge/commands.zig` (opcode enum, command union, packed 24-bit arguments, encode/raw helpers), `src/ge/types.zig` (primitives, enable registers, texture formats/wrap/filter, blend/depth/stencil, matrices), `src/gu/pspgu.zig` + `src/gu/command_buffer.zig` (exact `sceGu*` → GE packet encoding and GU state), `src/ge/queue.zig` (list queue/sync states). Auto-generated `src/c/module/*.zig` ABI/structure declarations (ATRAC, MPEG, audio, utility, I/O) are a **secondary cross-check** for T6, not a source of truth. Writers `tools/{prx,sfo,pbp}` are not needed; `tools/prxencrypt` (KIRK/PRX encryption) is out of scope and never vendored. |
| **andrewcirincione/Rift-Ascent** | None (no LICENSE; all rights reserved) | **No code or asset adoption.** Freestanding PSP homebrew (PSPDEV, custom `_start`, user-mode import stubs, software renderer into EDRAM) with source + EBOOT releases and strong QA (ASan/UBSan, deterministic native campaign player, PPSSPP fixtures, exact binary digest). Only an optional local-only recompilation corpus; its custom startup and software path are unrepresentative of Gungnir, and its generated art is huge. Not a dependency; never commit its EBOOT/assets. |
| **Jstar269/hst-decomp** | GPL-3.0 | Manifest/data discipline: `modules.json` / `target-manifest.json` (hash-only disc identity, load segments), hex `start end` function lists, `milestones.json` ratchet ("never lower a figure"), toolchain-lock re-verification, REUSE.toml licensing |
| **jessicanataliagta/PSPRecomp** | MIT | **The framework core we adopt**: `include/psprecomp/` (decoder, elf32, guest_memory, program_analysis, nid_registry, runtime), profile/config split (`[policy]` strictness knobs: `missing_function=stop`), framework/profile architecture docs. ⚠️ Its `profiles/vcs/generated/` (108 MB translated game code) is the #1 anti-pattern to invert — never commit generated output |
| **TeamGDB/Yakumo** | MIT (+GPL quarantine) | Most mature framework consumer: **overlay runtime-swap design** (`unregister_functions` + missing-function hook + interpreter fallback — the answer to PSP PRX overlays), SAF document picker on Android, touch overlay + editor, savedata crypto (FIPS-197 self-tested), SHA-256-pinned installer ("accept exactly one known input"), `SOURCE_PROVENANCE.md` template, pinned FFmpeg build, multi-platform CI/release automation. Keeps `generated/` gitignored — the policy we follow. **ADOPTED (T3):** `elf32.cpp`, `iso_image.cpp`, `param_sfo.cpp`, `utf8_path.cpp`, `common.hpp`, `guest_memory.hpp` — MIT framework subset vendored byte-identical at `third_party/yakumo/` (commit `b61852a`), ISO reader being the azazel6-identified reusable seam |
| **zxyandreay/azazel6** | MIT | **`tools/repository_check.py`** (extension + magic-byte + blob-size + ignore-rule + secret audit — adapt as our CI gate from day one), bounded-run discipline (dispatch + wall limits; never claim synthetic runs as game runs), evidence labels (VERIFIED/HYPOTHESIS/…), `dependencies.json` pin pattern, sectionless-PSP-ELF handling, Vulkan/input/lifecycle **probe APK before any game code**, pixel-readback contract tests |
| **sp00nznet/psprecomp** | MIT | Oracle discipline ("diff against, never link"), KIRK/decrypt docs **without keys** (`psp_keys.example.txt` placeholder pattern), function-discovery methodology (R_MIPS_32 pointer mining, init/fini arrays), code-outside-`.text` warning (ctors in `.data`), bring-up journal culture, MIT HLE/runtime reference |
| **sp00nznet/wtf-psp-recomp** | MIT | Per-game repo layout: toolkit as pinned dependency, tiny hand-written `host.c`, `CONFIGURE_DEPENDS` glob over gitignored `gen/`, `docs/NOTES.md` findings journal with logged corrections. ⚠️ Its stated *intent* to commit generated C later is an anti-pattern we reject |
| **wizardengineer/psprecomp** | GPL-2.0-or-later | Pipeline shape: Ghidra census → `analysis.json` single source of truth → typed IR → C++17 emitter; **jump-table recovery** via DATA xrefs; BIDS (branched-into delay slots); fingerprint staleness gates + `--expect-functions` census gates; **facts-vs-choices config split** (`game.toml`); `purity_gate` (no game literals in runtime core) |
| **anthonyhfm/psprism** | GPL-3.0 | **Interpreter fallback for dynamic code** (the only surveyed recompiler that has one — required design), code-map format (`entry/function_range/block/gp/t9/overlay/exclude`), "hydrate" bring-your-own-game workflow, `LICENSING.md` boundary doc. ⚠️ Its committed `ppsspp_niddb.xml` lacks attribution — don't copy data tables without provenance |
| **sal063/PSP-recompilation-project** | Other (mixed) | **Validation layer to copy wholesale**: software rasterizer as parity oracle, PPSSPP-headless golden traces (`microtest_gate`/`codegen_gate` layering), hand-computed self-tests, VFPU differential fuzzer + prefix/quirk model, PGF parser reference, per-`rt/` component map. ⚠️ Anti-patterns: committed Sony PGF fonts, committed `SDL3.dll`, LUT provenance gap |
| **snuri00/psp-web-recomp** | MIT | **The 10-patch bug-class catalogue** (see Gotchas §7C) — the pre-mortem for our codegen; interpreter + hashed code-cache as permanent tier; PRX translate/unload; `at3_standalone` LGPL ATRAC3 decoder. ⚠️ Anti-pattern: hardcoded KIRK key material in `pgd.cpp` |
| **kreycai/gungnir-ptbr** | None | **All Gungnir format knowledge** (DAR/TPL/text/script/EBOOT layout — §2), repack in-place rules, round-trip byte-identical proof discipline, ink-width font calibration. Use the *format facts*, never the translated strings/JSON/assets |
| **codepdbh/PSPRecomp-VCS-Android** | MIT | **Proven Android build pins**: AGP 8.10.1, Gradle 8.11.1, compileSdk 36, minSdk 26, NDK 28.2.13676358, CMake 3.22.1, `c++_static`, C++20; AAudio low-latency path; CPU affinity/priority tuning (incl. SELinux fallback); force `-O2` on generated TUs even in debug; per-unit opt-level overrides for NDK-Clang-choking units. ⚠️ Anti-patterns: 108 MB committed generated corpus, `MANAGE_EXTERNAL_STORAGE`, CPU-readback present path |
| **alexandercorp287/GTAViceCityStories-Android** | MIT | Touch-overlay UX ideas (floating stick, hide-on-pad, layout editor) — **reimplement behaviorally, never copy** (reVC-derived influence is GPL-tainted); EGL backend attempt (dead code — informs our "one backend interface" requirement). ⚠️ Anti-patterns: no `.gitignore`, committed `local.properties`, Google Drive "ready-made game files" link, in-repo screenshots |

## 4. Architecture & repo layout

```
gungnir-recomp/
├── AGENTS.md                    # single policy authority (hard limits, evidence rules)
├── LICENSE                      # GPL-3.0-or-later
├── .recomp.json                 # recomp.fyi card
├── .gitignore                   # game data / keys / generated code, each with rationale
├── dependencies.json            # pinned third-party revisions (azazel6 pattern)
├── CMakeLists.txt               # desktop + Android shared build (nk_add_title_runtime shape)
├── cmake/Deps.cmake             # fetch-or-reuse with lock+digest
├── configs/nids.csv             # NID registry (BSD pspsdk headers / fetch-with-checksum)
├── tools/
│   ├── repository_check.py      # IP gate (azazel6 adaptation) — CI, pre-push
│   ├── package_notices.py       # generated THIRD_PARTY_NOTICES.txt from one JSON
│   ├── intake/                  # chd→iso→extract→decrypt-hook→manifest
│   ├── analyze, codegen         # analysis + AOT (adopted MIT core + our glue)
│   ├── dar.py, tpl.py           # DAR/TPL readers (user's local files only)
│   ├── textures/                # dump/replace + font-atlas repack
│   └── ghidra/ExportPSPFunctions.java   # optional function census
├── runtime/                     # guest memory, dispatch, interpreter, HLE, GE, post
│   ├── hle/  ge/  post/  input/  audio/  savedata/
│   └── platform/adreno/         # custom Vulkan ICD loader
├── profiles/gungnir/
│   ├── config.toml              # facts-vs-choices (wizardengineer split)
│   └── local/                   # GITIGNORED: hashes, address pins, per-dump data
├── android/                     # Gradle (Kotlin DSL) + JNI shell
├── assets/shaders/{ge,post}/    # committed GLSL (ours), SPIR-V embedded at build
├── assets/input_profiles/       # handheld gamepad profiles (ours)
├── tests/fixtures/              # synthetic Allegrex fixtures of OUR authorship
├── data/milestones.json         # hst-decomp ratchet ledger
├── docs/                        # see §6 documentation checkpoints
└── build/                       # GITIGNORED: game tree, generated code, outputs
```

**Pipeline:** `game.chd` → [T3 intake] → `build/game/` (verified ISO, decrypted ELF, manifest) → [T5 analyze+codegen] → `build/generated/` (chunked C++ TUs) → [T6/T7 runtime] → Linux desktop boot → [T9/T10/T11 Android] → adb device → [T13] title screen → [T14] device sign-off.

## 5. Tasks

Each task: dependencies, work, deliverables, test cases (TC), docs checkpoint, reference callouts, gotchas (pointer to §7 register).

**Red-first rule for all tasks (§8.1):** each TC is written and observed red *before* the implementation that satisfies it; bug fixes begin with a reproducing red test; no task below closes with a red or disabled TC. Tasks touching runtime semantics also carry a mutation check.

### T1 — Scaffolding + IP guardrails
**Depends on:** — (first task)
**Work:** git init (keep `game.chd` untracked forever); `LICENSE`, `AGENTS.md`, `README.md`, `.gitignore` (case-insensitive blocks for `*.iso *.chd *.cso *.elf *.prx *.pbp *.bin *.pmf *.at3 *.vag`, `keys/`, `generated/`, `game/`, `PSP_GAME/`, `build/`, `profiles/*/local/`, analysis dumps — with rationale comments, nakagawa style); `tools/repository_check.py` (magic-byte sniff of git index for ELF/`~PSP`/CHD/PE, >1 MB blob rejection, ignore-rule regression tests, secret scan); `docs/LEGAL.md` (the IP charter = §0); `docs/SOURCE_PROVENANCE.md`; `.recomp.json`.
**Test cases:**
- TC-1.1 `repository_check.py` passes on clean tree.
- TC-1.2 Staging a dummy `.iso`/`.elf`/`~PSP` blob fails the check (each magic recognized).
- TC-1.3 Ignore-rule regression: creating `build/game/x.iso` never appears in `git status`.
- TC-1.4 `game.chd` in workdir is never matched by `git ls-files`.
**Docs checkpoint:** `docs/LEGAL.md` + `docs/SOURCE_PROVENANCE.md` + `AGENTS.md` written **before any code lands**.
**Refs:** nakagawa-recomp (`.gitignore`, AGENTS.md), azazel6 (`repository_check.py`), psprism (`LICENSING.md`), Yakumo (`SOURCE_PROVENANCE.md`).
**Gotchas:** §7A.

### T2 — Third-party dependency ledger + license machine
**Depends on:** T1
**Work:** `dependencies.json` pinning: MIT PSPRecomp framework core (choose jessicanataliagta/PSPRecomp `include/+src/` or Yakumo framework subset — record exact commit + provenance), CHD reader (libchdr or chd-rs — audit first), SDL3 (pinned commit, nakagawa `deps.cmake` pattern), stb image, tiny-AES-class public-domain crypto (savedata later), NID name data (BSD pspsdk headers or `pspdev/psp-ghidra-scripts` fetch-with-checksum — **never** copy psprism's unattributed `ppsspp_niddb.xml`; PPSSPP-derived tables are usable under our GPL-3.0 **with attribution**); `cmake/Deps.cmake` (fetch-or-reuse, lock+digest, offline-capable); `THIRD_PARTY_LICENSES/`; `tools/package_notices.py` → `THIRD_PARTY_NOTICES.txt`.
**Test cases:**
- TC-2.1 Clean checkout + one command materializes all deps at verified commits (digest check fails on wrong revision).
- TC-2.2 `package_notices.py` output covers every vendored file (CI gate: no unlisted file under `third_party/`).
- TC-2.3 License gate rejects a deliberately added CC BY-NC-SA component.
**Findings (2026-10-30):** first pin landed early with T3: `dependencies.json` + `third_party/yakumo/PROVENANCE.md` + in-tree LICENSE (MIT, pin `b61852a`) — the template the remaining entries (SDL3, CHD reader if ever needed, stb, ATRAC decoder) must follow. **License-implication of §1 Code reuse:** clean-room reimplementations are not sanctioned — where no adoptable code exists for something we need, **stop and ask** (that decision belongs to the user, not the agent).
**Docs checkpoint:** `docs/PROVENANCE.md` (per-subtree: upstream, commit, license, disposition).
**Refs:** azazel6 (`dependencies.json`), nakagawa (`deps.cmake` + notice generation), wizardengineer (`fetch-niddb.sh` + checksum), psprism (attribution gap as cautionary tale).
**Gotchas:** §7A.5–A.7.

### T3 — Game intake pipeline (`game.iso` → verified local build tree)
**Depends on:** T1
**Work:** `tools/intake/`: (1) **disc input = ISO only** (locked decision §1: `game.chd` is converted user-side via [CHDStudio](https://github.com/purelogiccode/CHDStudio) or `chdman extractcd`; intake refuses `.chd` with a named error pointing at `docs/INTAKE.md`; we never parse CHD — the identity gate *verifies, not trusts*, the converter); (2) identity gate: size 262,766,592 + CRC32 `183F3EDC`, **fail closed** on mismatch; (3) ISO9660 extract of `PSP_GAME/` tree (EBOOT.BIN, USRDIR `*.dar`, INSDIR, PARAM.SFO — verify disc ID ULUS-10592); (4) decrypt hook — **no decryption code in-repo**; invoke user-supplied pspdecrypt/PPSSPP "Dump decrypted EBOOT.BIN" out-of-process, accept `BOOT.BIN` plaintext or user-provided decrypted ELF, validate PRX shape (`e_type 0xFFA0`, `vaddr = file_offset − 0xC0`, reloc section `0x70000020`, types 2/5/6); (5) `build/game/manifest.json`: module list, SHA-256s, segments, reloc counts, NID import inventory. All outputs under `build/` (gitignored).
**Test cases:**
- TC-3.1 End-to-end on local `game.iso` produces manifest.json with expected module set. *(Covered by `tests/test_intake.py` — written and observed red before implementation.)*
- TC-3.2 Wrong-CRC input (bit-flipped ISO) aborts with named error `IDENTITY_MISMATCH`; `.chd` input aborts with named error `CHD_NOT_SUPPORTED` pointing at CHDStudio.
- TC-3.3 Truncated/decrypted-partially ELF rejected (magic + program-header checks): `~PSP` magic → `ENCRYPTED_EXECUTABLE` (points at out-of-process decrypt), truncated/garbage → `ELF_INVALID`.
- TC-3.4 Every intermediate artifact lands under `build/`; `repository_check` still green.
**Findings (2026-10-30, from implementation):** ISO9660 + ELF32 + PARAM.SFO parsing is **adopted** from TeamGDB/Yakumo (MIT, `third_party/yakumo/`, pin `b61852a`) — not clean-room, per §1 Code reuse. Intake shape is therefore a C++ reader tool (`tools/intake/intake_tool.cpp`, subcommands `iso-extract`/`elf-inspect`/`sfo-info`, built by root `CMakeLists.txt`) + Python orchestrator (`tools/intake/intake.py`: identity gate + manifest via stdlib `zlib`/`hashlib`/`json` — existing implementations). Known gap: vendored `elf32.cpp` handles reloc section `0x700000A0` only; Gungnir's EBOOT uses `0x70000020` — intake counts both via its own section walk (`ElfSection` list + raw entry scan), vendored code stays unpatched (recorded in `third_party/yakumo/PROVENANCE.md`). Manifest records the actual section type found, for T5.
**Docs checkpoint:** `docs/INTAKE.md` (user-facing: how to supply their own disc, external decrypt tools, what the gates check).
**Refs:** gungnir-ptbr (ISO offsets, `~PSP` layout — facts only), wtf-psp-recomp (`extract.ps1` report style), Yakumo (`prepare_game.sh` SHA-pin pattern), codepdbh (`--verify-only` identity gate).
**Gotchas:** §7B (format traps), §7A.1–A.3.

### T4 — Framework core integration + oracle harness (Linux desktop)
**Depends on:** T2
**Work:** Root CMake + `runtime/`: guest memory (32 MB RAM @ `0x08800000`, **16 KB scratchpad @ `0x00010000`**, VRAM @ `0x04000000`, cache-mirror collapse), full-coverage Allegrex decoder (incl. ADDI; tolerant data-as-code decode), AOT codegen to `build/generated/` chunked TUs (never committed; content-hash fingerprint staleness gate), dispatch with **interpreter fallback + FNV block cache as a permanent tier**, forward-branch yield counters. Test strategy: committed synthetic Allegrex fixtures (our authorship) run under interpreter AND AOT with trace equality; hand-computed self-tests (sal063 pattern — expectations never generated from the implementation); `ctest` on Linux x86_64.
**Test cases:**
- TC-4.1 All synthetic fixtures: interpreter trace == AOT trace (byte-equal).
- TC-4.2 Deliberately broken fixtures (bad opcode, dispatch miss, stack imbalance) fail loudly — no silent fallback.
- TC-4.3 `git ls-files` contains zero generated units; regenerate-twice is byte-stable (fingerprint gate).
- TC-4.4 VFPU differential fuzz: codegen-emitted VFPU vs interpreter VFPU, randomized states, bitwise compare (`vi[]` + float bits) — zero diffs on the supported op set.
**Findings (2026-10-30):** root `CMakeLists.txt` already seeded (T3) with the `yakumo` static lib + `intake_tool` target — T4 extends it. Guest memory semantics arrive with the adopted `guest_memory.hpp`; **VFPU is the one place a clean-room impulse is likely — it is not sanctioned**: adopt an MIT VFPU (sp00nznet `vfpu.c` is MIT; sal063's is mixed-license — audit first) or stop and ask before writing one.
**Docs checkpoint:** `docs/ARCHITECTURE.md` (pipeline + memory map + dispatch contract), `docs/TESTING.md` (oracle hierarchy).
**Refs:** jessicanataliagta/PSPRecomp (core), sal063 (golden-trace layering, VFPU fuzz), snuri00 (interpreter+cache, the 10 bug classes), wizardengineer (fingerprint/census gates), sp00nznet (oracle discipline).
**Gotchas:** §7C (recompiler bug classes).

### T5 — Gungnir profile: static analysis + AOT generation
**Depends on:** T3, T4
**Work:** `profiles/gungnir/`: function discovery = entry + `jal` targets + **R_MIPS_32 relocation mining** (stored function pointers — wtf found 2,633 on one module) + **HI16/LO16 pair seeding** + prologue-after-return heuristic + init/fini arrays + code-outside-`.text` awareness (ctors in `.data`); jump tables via Ghidra DATA-xref export (optional tool) with runtime dispatch fallback; `config.toml` facts-vs-choices; all hashes/address pins in gitignored `profiles/gungnir/local/`; policy `missing_function=stop`, `unsupported_instruction=stop` (no silent stubs). Input: Gungnir EBOOT + PRXs from T3.
**Test cases:**
- TC-5.1 Analysis manifest covers 100% of executable bytes in the import inventory (census gate: `--expect-functions`/`--expect-instructions` bounds asserted, not eyeballed).
- TC-5.2 Codegen emits compilable chunked TUs; repeated run byte-stable.
- TC-5.3 Mutation gate: breaking one instruction semantics kills the synthetic-fixture gate (mutations count only when they kill for the right reason — hst-decomp rule).
- TC-5.4 Dispatch-coverage report from a bounded run: every executed address maps to a translated function or an explicit interpreter fallback (counted, not silent).
**Docs checkpoint:** `docs/ANALYSIS.md` (discovery seeds, coverage numbers), `docs/BRINGUP.md` **journal opened** (symptom→falsification→root-cause entries with retractions logged, sp00nznet/hst style).
**Refs:** wizardengineer (analysis.json + jump tables + census gates), wtf-psp-recomp (reloc mining), hst-decomp (function list formats, blocked-functions ledger), azazel6 (bounded runs, sectionless ELF), gungnir-ptbr (EBOOT reloc types 5/6 facts), psprecore (C17 codegen, jump-table recovery, PRX load — cross-check only; MIT).
**Gotchas:** §7C.3–C.8.

### T6 — HLE kernel bring-up (Linux desktop)
**Depends on:** T4, T5
**Work:** `runtime/hle/` boot set: SysMem, ThreadMan (cooperative first — consensus of all surveyed repos — real-threads seam like sp00nznet's `PSPRECOMP_REAL_THREADS`), InterruptManager, IoFileMgr (ISO9660 + `sce_lbn` raw extents), Display + VBLANK @ 59.94 Hz pacing, sceGe command submission, sceCtrl, LoadExec, **ModuleMgr with PRX overlay tiers** (translate-on-load, `unregister_functions` on unload — Gungnir logs `sceKernelLoadModule`), scePower, sceRtc. Unknown-NID handling: **fail-closed loud trap** naming `NID_0x…` + registry name — never silent 0-returns (azazel6 rule). Unknown-but-required NIDs discovered by executing the game's own shipped modules early (LLE-first where guest code exists — nakagawa rule). `data/milestones.json` ratchet ledger (hst-decomp format).
**Test cases:**
- TC-6.1 HLE unit tests: threads/semaphores/eventflags/alarms/IO read paths.
- TC-6.2 Bounded Linux boot of recompiled Gungnir passes module init + first `sceDisplay` set; captured trace lists zero silent NIDs (any unknown = counted failure).
- TC-6.3 PRX overlay load→unload→reload preserves address space (no leaked function registrations).
- TC-6.4 Thread-scheduling determinism: two bounded runs with the same inputs produce identical traces (or the divergence point is itemized).
**Docs checkpoint:** `docs/HLE.md` (module/NID coverage table), `docs/BRINGUP.md` journal entries for every boot blocker.
**Refs:** Yakumo (kernel + overlay swap), sp00nznet (hle/ module set), wizardengineer (bind-by-name syscall tables), snuri00 (kernel.cpp/modules.cpp), sal063 (hle.c 201-NID reference), psprecore (kernel, savedata, SAS/ATRAC, MPEG semantics — MIT; its save layout omits our `PSP/` prefix).
**Gotchas:** §7C.6, §7D.

### T7 — Vulkan GE renderer + post-processing chain
**Depends on:** T4 (parallel with T5/T6)
**Work:** `runtime/ge/` behind one backend interface: CPU front end (display-list decode, all PSP vertex formats, T&L with VFPU-accurate transforms, clipping) → **Vulkan 1.1 backend** (screen-space ubershader; GLSL in `assets/shaders/ge/` → embedded SPIR-V via `tools/embed_shaders.py`; pipeline cache keyed by blend/cull/z; texture cache keyed by state + content hash **including CLUT**; correct `bufw` handling; render-to-texture feedback invalidation). Software rasterizer retained as parity oracle (sal063/codepdbh pattern). VFPU: MIT implementations (sp00nznet `vfpu.c` + sal063 prefix model) + differential fuzz (TC-4.4); transcendental LUTs either adopted from PPSSPP under GPL attribution or computed-and-diffed. `runtime/post/`: composable passes (integer scale, bilinear + FSR-lite upscale, **LCD-grid/scanline presets**) selectable via user preset JSON (`assets/shaders/post/`), applied after GE compositing. On Android present via real swapchain — **not** the CPU-readback `ANativeWindow` path used by codepdbh (perf trap).
**Test cases:**
- TC-7.1 GE fixture suite via pixel-readback contract tests (azazel6 pattern): triangles, strips/fans, all texture formats incl. CLUT4/8/16/32, blend modes, scissor, depth, render-to-texture.
- TC-7.2 Software-vs-Vulkan parity: fixture renders match within documented tolerance (integer paths exact).
- TC-7.3 Post preset changes output in a pinned screenshot hash (our own fixture scene — no game frames in-repo).
- TC-7.4 Feedback path (render to area containing texture) invalidates cache — targeted regression from Gungnir's PPSSPP quirk log.
**Docs checkpoint:** `docs/RENDERER.md` (backend interface, texture cache keys, known approximations), `docs/SHADERS.md` (preset format, authoring rules).
**Refs:** sal063 (`ge.c` + `gpu_sdl3vk` ubershader + CLUT- hash), codepdbh (Vulkan backend shape, present-path caution), wizardengineer (bufw bug class, SIGNAL gaps), Yakumo (`vulkan_renderer.cpp`), snuri00 (surfaces/stale-rects), **psprecore** (MIT: `runtime/src/ge.cpp` walker, vertex pipeline, `soft_renderer.cpp`, `texture.cpp`, `gpu_vulkan.cpp` — primary T7 behavioral/port reference), **ff7ccrecomp** (MIT: shipped Android Vulkan app/package shape).
**Tracking:** work is decomposed in dex into T7a–T7h (decoder, vertex/clip,
software oracle, textures, Vulkan first triangle, draw path, post, closure).
Each starts with the repository code-search/issue-search reference step and
records what it used.
**Gotchas:** §7D.2–D.4, §7E.5.

### T8 — Texture pipeline: dump/replace + TPL decode + text upscaling
**Depends on:** T3, T7
**Work:** (1) TPL decoder (0x30 B header, per-texture `H/W/BW/bpp/data_off`, 4bpp swizzle, 8bpp+CLUT) documented abstractly in `docs/formats/TPL.md` — written from the user's local files, **no TPL bytes committed**; (2) dump mode: hash-keyed PNG export of every decoded texture (PPSSPP `textures.ini` semantics) to user storage; (3) replacement loader: user PNGs at any resolution take precedence (the upscaled-texture path); (4) `tools/textures/repack_font_atlas.py`: re-rasterize dialog font atlas at Nx from a user-supplied glyph source, regenerate ink-width advance table, repack with **round-trip byte-identical invariant** (writing the original back through our writer must reproduce the input — gungnir-ptbr proof #8). Two fronts because Gungnir text lives both in font atlases and baked into UI images (actname/tutorial/location TPLs) — image replacement covers the second.
**Test cases:**
- TC-8.1 Synthetic TPL fixtures (our authorship) decode→re-encode byte-identically.
- TC-8.2 Dump→replace→reload in the Linux runtime with a test scene (no game assets in-repo).
- TC-8.3 2x/4x font atlas renders a fixed test string without glyph collisions; width-table regen passes ink-width assertions.
- TC-8.4 Swizzle edge cases: odd widths, multi-texture containers (`n_tex > 1`), `bpp=4` alpha ramp and `bpp=5` CLUT decode.
**Docs checkpoint:** `docs/formats/TPL.md`, `docs/formats/DAR.md` (from T3 knowledge), `docs/TEXTURE_REPLACEMENT.md` (user guide: dump layout, naming, scaling rules).
**Refs:** gungnir-ptbr (TPL/DAR/font metric facts, round-trip proof discipline), sal063 (`pgf.c` parser reference for future sceLibFont), psprism/Yakumo (texture-cache + replacement precedents), psprecore (texture collection/replacement matching and HD layering — MIT), marco-calautti/Rainbow (`TPLTexture.Serializer.cs`: multi-image table and named-error structure for the separate Nintendo TPL format — not the Gungnir PSP variant), LiinkPK/Texture-Replacement-Tool-for-PPSSPP (replacement-pack management and `textures.ini` UX; binary-only, no source adopted), PPSSPP PR #8715 + wiki (textures.ini sections, `[options]`, `[hashes]`, `[hashranges]`, hash behavior for T8c/T8d).
**Tracking:** T8a–T8f complete (TPL decoder, byte-identical writer, hash-keyed dump, replacement loader, font-atlas repack tool, consolidated TC-8.1–TC-8.4 gate). Named gaps and mutation evidence are in `docs/TEXTURE_REPLACEMENT.md`; DAR extraction/repack remains T12.
**Gotchas:** §7B.2–B.4, §7F (font width collision, baked-in text).

### T9 — Android app shell + adb deployment
**Depends on:** T4, T7
**Work:** `android/` Kotlin-DSL Gradle (pins reconciled 2026-10-30 with this host and Google's AGP compatibility table: AGP 8.10.1, Gradle 8.11.1 SHA-pinned wrapper, JDK 17, NDK 28.2.13676358, CMake 3.22.1, build-tools 36.0.0, compileSdk 36, minSdk 26, targetSdk 35, `arm64-v8a`+`x86_64`, `c++_static`, C++20; AGP 8.10's maximum API is 36 and it requires Gradle 8.11.1, so nakagawa's newer AGP 9.5/Gradle 9.8.2 pins are deliberately not adopted). One shared lib via `nk_add_title_runtime`-style CMake function (desktop/Android share sources — no runtime fork). SDL3 Activity shell (gamepad/hotplug/rumble/touch + Vulkan surface). Machine paths only via `-P` properties (never committed; no `local.properties`). Game data: SAF document-tree picker + `adb push` to `getExternalFilesDir()` (two documented paths); first-run asset unpack with version stamp; **runtime hash gate enforced** (not merely logged). Deploy scripts `scripts/android/{build,install,run,screenshot,logcat}.sh`. Force `-O2` on generated TUs even in debug + per-unit opt overrides for NDK-Clang-choking units; CPU affinity/priority tuning (pin after Vulkan device creation; SELinux-safe fallback).
**Test cases:**
- TC-9.1 `build.sh` produces an APK from a clean checkout (no machine-local paths required beyond `-P` flags).
- TC-9.2 `adb install -r && run.sh` boots the synthetic-fixture runtime to a rendered Vulkan frame on device/emulator.
- TC-9.3 Both game-data paths work (SAF pick and adb push) and the hash gate rejects a corrupted image.
- TC-9.4 Lifecycle: background→foreground resumes the guest between dispatches from the pc it stopped at, keeps one `session_start`, leaves progress intact and recovers rendering, audio and input; a run that ended for its own reason is never revived. Rotation ignored (sensor-landscape lock).
**Docs checkpoint:** `docs/ANDROID.md` (setup, adb/SAF flows, device storage layout, emulator notes).
**Refs:** nakagawa-recomp (`android/` skeleton — primary template), codepdbh (proven pins, AAudio, affinity), Yakumo (SAF picker), azazel6 (probe-APK-first approach — validate shell on emulator before game code), ff7ccrecomp (shipped psprecore Android app: `minSdk 29`, NDK 29, SDL3 Java, `useLegacyPackaging`).
**Tracking:** work is decomposed in dex into T9a–T9f (build skeleton + pins, shared-runtime JNI + SDL3 probe APK, SAF/adb intake + hash gate, lifecycle/audio/rotation, NDK tuning + CPU affinity, deploy scripts + docs + device QA closure). Each starts with the repository code-search/issue-search reference step and records what it used.
**Gotchas:** §7E (Android platform traps).

### T10 — Adreno custom Vulkan driver loader
**Depends on:** T9
**Work:** `runtime/platform/adreno/` + Android glue: user picks a driver `.zip` (Mesa Turnip-style Vulkan ICD package) in settings or drops it in a documented folder; extract to app-private storage; validate (`meta.json` soname or a bare `libvulkan.so`, ELF machine matches app ABI); load through libadrenotools (BSD-2-Clause; adopted in T10a instead of a clean-room loader) that `dlopen`s the ICD instead of the system one. Per-driver selection + version display; "system driver" default; fail-closed fallback with a named error. Driver zips/blobs: **user-supplied, gitignored, never bundled** (repository_check excludes the driver directory).
**Test cases:**
- TC-10.1 Stock-driver boot unaffected on any device (loader bypass path).
- TC-10.2 On an Adreno device, a user-supplied zip loads and reports its version via logcat.
- TC-10.3 Malformed zip / wrong-ABI ELF fails closed with clear error, app still boots on system driver.
- TC-10.4 `repository_check` rejects a `.zip`/`.so` committed under the driver paths.
**Docs checkpoint:** `docs/ANDROID_DRIVERS.md` (what zips are, where to get them, risks, selection UI).
**Refs:** libadrenotools (adopted, BSD-2-Clause), Vita3K/Yuzu-family driver-zip UX (behavioral reference only), nakagawa (`deps.cmake` pin/digest discipline applies to any shim we adopt), ff7ccrecomp (`useLegacyPackaging` and libadrenotools native-library loading).
**Tracking:** work is decomposed in dex into T10a–T10f (package contract + adoption decision, host package inspector, ICD loader + runtime dispatch, Android import UI + packaging, fail-closed matrix + repository guard + device QA, docs + closure). Each starts with the repository code-search/issue-search reference step and records what it used.
**Gotchas:** §7E.6–E.7.

### T11 — Input + haptics
**Depends on:** T9 (T6 for sceCtrl surface)
**Work:** `runtime/input/` + the Android touch overlay: (1) touch overlay — floating analog stick, d-pad, face buttons, L/R, START/SELECT, editable drag/resize layout persisted as JSON; **the overlay is a native Android `View` above the SDL surface (decision 2026-10-31), not an ImGui/Vulkan overlay**: a host-testable C++ layout/pointer/editor model in `runtime/input/touch_overlay.*` with a thin `TouchOverlayView` drawing/input layer in `android/app/src/main/java/com/gungnir/recomp/input/`, and a JNI publish of the merged touch mask/analog into `runtime/input/`. Yakumo's MIT `touch_controls`/`touch_editor` are the behavioral model to port (pinned commit, provenance; GPL projects are behavioral references only — **no GPL code or assets copied**); each button/d-pad touch-down fires one short haptic tick through `View.performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)` after an explicit `Settings.System.HAPTIC_FEEDBACK_ENABLED` check, the analog stick and overlay drag/resize stay silent, and a missing vibrator is a no-op; (2) gamepad backend (SDL3 gamepad API; shipped name-matched profiles in `android/app/src/main/assets/input_profiles/profiles.json` for Ayn/Ayaneo-class built-ins; per-GUID user profiles under app-private `input_profiles/`; a Gamepad page in the Menu settings overlay with a non-focusable remap wizard for 12 buttons plus the left-stick X/Y axes, Save and Reset; the touch overlay hides while a pad is connected and returns on disconnect); (3) keyboard/mouse for desktop dev; (4) haptics: **touch-control feedback only in T11**. No-op when haptics are disabled or the device has no vibrator; a failed vibration never blocks input. Physical gamepad rumble may be plumbed through SDL3 but is not driven by Gungnir yet. `sceRumble`-class game/battle haptics are deferred to T13/M5 after bring-up discovers the title's actual usage.
**Test cases:**
- TC-11.1 Input unit tests: physical events → sceCtrl bitmask/analog mapping table.
- TC-11.2 On-device: touch overlay navigates the synthetic fixture; paired gamepad navigates the same; overlay hides when pad active.
- TC-11.3 Touch haptics: host policy tests prove a button/d-pad down requests exactly one short tick while stick movement, drag/resize and release request none; disabled or absent vibrator is a no-op; on the physical phone each touch control press produces a short tick and input still works. The x86_64 emulator may expose a virtual vibrator; the no-vibrator/no-op path is covered by the host absent-sink test and by handling `performHapticFeedback` returning false. Gamepad rumble and battle haptics are not T11 acceptance.
- TC-11.4 Profile round-trip: remap → save JSON → reload → identical binding.
**Docs checkpoint:** `docs/INPUT.md` (overlay editor, profiles, key maps, touch-haptics behavior, system-setting fallback, and the deferred battle-rumble note).
**Refs:** **TeamGDB/Yakumo (MIT)** is the primary behavioral source for the touch model: `profiles/mhp3rd/host/input/touch_controls.*` (floating stick, dpad_buttons, stick_deflection, pointer roles/multi-touch), `ui/touch_editor.*` (drag/resize, reset) and `tests/touch_controls_tests.cpp`. Port the semantics to Java for the Android `View`; record the pinned commit and provenance, and copy no code. Other behavioral references (GPL; no code or assets copied): PPSSPP (layout editor, pointer capture), Dolphin (Android input overlay/hide), RetroArch (overlay hit-testing), Flycast (Android virtual gamepad), reVC/re3 (mobile touch UX). Platform APIs: Android `View`/`Canvas`/`MotionEvent`/`WindowInsets`, `VibratorManager`/`VibrationEffect`/`HapticFeedbackConstants`; SDL3 gamepad for T11d.
**Gotchas:** §7E.8 (GPL-tainted UX lineage), §7F.6 (Gungnir rumble unknowns; touch haptics land in T11, battle mapping later).

### T12 — Audio + savedata + DAR asset support
**Depends on:** T6
**Tracking:** split in dex into T12a (audio) and T12b (savedata + DAR assets); the parent closes when both are complete and T13 stays blocked until then.

#### T12a — Audio: SAS/ATRAC/MP3 synthesis and device output
**Depends on:** T6 (T6h ATRAC decoder, T6g/T6m SAS slice, T6f sceAudio channels already landed)
**Work:** `runtime/audio/`: finish sceSasCore 32-voice VAG/ADPCM synthesis (pitch/ADSR; close the remaining voice/effect calls the title executes), complete the sceAtrac3plus track calls over the vendored FFmpeg n4.4 decoder (T6l left six track calls as a counted gap; never fake PCM), sceMp3 (adopted decoder or a named counted gap), mixed to one 44.1 kHz stream → AAudio low-latency callback (Android) / SDL audio (desktop). Ring buffer, fade and error-callback restart; underrun accounting.
**Test cases:**
- TC-12.1 SAS voice unit tests (ADPCM decode, pitch, ADSR envelope shapes).
- TC-12.2 Audio smoke: tone + VAG playback via AAudio on device, SDL on desktop; no underruns in a 60 s soak at 59.94 pacing.
**Docs checkpoint:** `docs/AUDIO.md` (voice model, codec coverage, sink contract, underrun policy, counted gaps).
**Refs:** sp00nznet (sascore), sal063 (audio.c), snuri00 (at3_standalone + SAS), codepdbh (AAudio path), FFmpeg n4.4 (vendored decoder; LGPL notices/relink), psprecore (SAS envelopes, ATRAC/MPEG — MIT; no runtime ATRAC decode, it cooks offline).
**Gotchas:** §7E.4 (AAudio underruns), §7F.5 (ATRAC edge-case decode quirk).

#### T12b — Savedata and DAR asset support
**Depends on:** T6 (T6k savedata SIZES/dialog state machine already landed)
**Work:** (1) `runtime/savedata/`: virtual memstick `PSP/SAVEDATA/<title>/` with PARAM.SFO v1.1 (PSF) read/write and a PPSSPP-compatible layout (saves interchangeable with emulator/hardware); complete the savedata modes the title executes beyond the T6k SIZES slice; KIRK save encryption **deferred**, documented honestly (hash-left-zero stance — never ship key material); (2) `tools/dar.py` for the asset pipeline (both DAR sides: read/list/extract + in-place rewrite rules; `stored_size==0` = uncompressed); size-preserving in-place rewrite with a zero-filled tail and named errors.
**Test cases:**
- TC-12.3 Save→load round-trip byte-stable; PARAM.SFO opens in PPSSPP.
- TC-12.4 `dar.py` lists/extracts the user's `ini.dar`; a repack round-trips byte-identically; the `stored_size==0` entries survive.
- TC-12.5 PPSSPP memstick interchange: copy the user's exported saves under `savestates/` (the SYSTEM and STORY save directories) into a scratch `ms0:/PSP/SAVEDATA/` root, list both through the virtual memstick, parse each PARAM.SFO, and parse→serialize each file byte-identically; save→load→save leaves every save file byte-identical and the export is never mutated. A runtime-written save is listed and loadable by PPSSPP.
- TC-12.6 PPSSPP oracle: a PPSSPP 1.20.4 clean boot (the supplied `.ppst` resumes before a battle, where no save menu exists) lists and loads the exported SYSTEM/STORY saves (`ULUS10592GUN9THSYS`, `ULUS10592GUN9THSTORY00`, 61,888-byte STORY) and also loads the tree rewritten by our store; every rewritten file is SHA-256-identical to the export. The `.ppst` container is never parsed in-repo (a reimplementation needs explicit sign-off); evidence is in `docs/qa/2026-10-31-t12b-savedata.md`.
**Local fixtures:** the savestate and both memstick saves are the user's own data under gitignored `savestates/`, never committed, and copied to a temp memstick for tests; needing cases skip with the named reason `savedata_fixture_missing`.
**Docs checkpoint:** `docs/SAVEDATA.md`, `docs/formats/DAR.md` (extended from T3/T8).
**Refs:** sal063 (savedata.c/PARAM.SFO), gungnir-ptbr (DAR traps), psprecore (savedata modes/focus/status — MIT; its save layout omits the `PSP/` prefix), PPSSPP (PARAM.SFO/save-layout oracle, behavioral only — no code).
**Gotchas:** §7B.1 (DAR `stored_size==0`), §7B.2 (in-place rewrite), §7B.3 (script `.bin` sizes), §7B.4 (both-endian ISO sizes).

### T13 — Milestone integration: title screen on device + CI/release hygiene
**Depends on:** T5, T6, T7, T8, T9, T10, T11, T12
**Tracking:** decomposed in dex into T13a–T13e, T13h and T13i (Android runtime session + AOT/HLE/audio/savedata wiring; title screen and menu at 1x with 59.94 Hz pacing and both input paths; BUILDING/ANDROID docs; CI build matrix with fail-closed private inputs; hygiene gates incl. dummy-artifact block, secrets, license/notice and SPDX/REUSE; milestone ledger, release checklist, AGENTS ratification and closure; one tracked new-game defect). The manual QA campaign, rumble audit and release sign-off live in T14. Each subtask starts with the repository code-search/issue-search reference step and records what it used.
**Work:** Wire the full chain: intake → AOT → APK → `adb install`/push → boot to Gungnir title screen with textures/text rendering (replacement path at 1x), touch + gamepad navigating the title menu, bounded-run logs clean of unknown NIDs, screenshot captured via `scripts/android/screenshot.sh`, hashed into a **gitignored** local milestone report (public `data/milestones.json` carries counts/criteria only — no game screenshots in-repo). CI: GitHub Actions building Linux runtime + Android cross-compile (synthetic fixtures only), `repository_check` + secret scan + license/notice gate + SPDX/REUSE lint, actions SHA-pinned, `exit 77` fail-closed skips for private-input tests. `docs/BUILDING.md` (from-scratch user workflow), `docs/ANDROID.md` finalized, `AGENTS.md` ratified. **The manual QA campaign, device matrix, soak, rumble audit and release sign-off are T14**, not this task.
**Test cases:**
- TC-13.1 CI green on a clean clone with zero game artifacts present (and `repository_check` proves it).
- TC-13.2 Deliberate dummy-ELF/ISO commit attempt blocked locally and in CI.
- TC-13.3 The documented build/deploy flow matches the scripts and gates (`tests.test_docs_building`).
- TC-13.4 Milestone ledger and public status recorded with measures and evidence pointers; a figure drop stays visible (never lowered silently).
**Docs checkpoint:** `docs/BUILDING.md`, `docs/ANDROID.md`, `docs/PROGRESS.md` (public status — counts and criteria only), release checklist in `docs/RELEASE.md`. The QA script and session reports are T14.
**Refs:** nakagawa (CI fail-closed + hygiene matrix), hst-decomp (milestones ratchet), azazel6 (evidence labels), Yakumo (nightly/release automation), ff7ccrecomp (frontend, recordings/replays, regression-script shape — MIT).
**Gotchas:** §7A.4 (public claims vs tree), §7E.1.

### T14 — Manual device validation and release sign-off
**Depends on:** T13a–T13e (engineering, docs and CI) and T13i where the first gameplay transition is required.
**Tracking:** decomposed in dex into T14a (manual QA campaign), T14b (rumble/actuator audit and M5 decision) and T14c (milestone and release sign-off). Each subtask starts with the repository code-search/issue-search reference step and records what it used.
**Work:** Write `docs/QA.md` (standard + Gungnir-specific script: title → menu → first gameplay transition). Run the device matrix: x86_64 emulator (SwiftShader) plus at least one physical Adreno arm64 phone and one non-Adreno/handheld where available; ask the user for device access and interaction; use adb and `scripts/android/*`. Execute the documented from-scratch pipeline from `docs/BUILDING.md` and reach the title screen plus menu input on the device. File one dated `docs/qa/<date>-<device>.md` per session (model, Android version, GPU/driver and version, build SHA, tester, steps, expected/actual, pass/fail counts, screenshot SHA-256; images stay in gitignored `build/qa/`). Run three consecutive cold boots per device, suspend/resume cycles and a 30-minute soak on at least one physical device (crash/thermal/audio counters, recording hash, no frames). Fix findings red-first where host-testable, otherwise track an owned defect. Audit `sceRumble`/`sceCtrlSetActuator` usage and either implement it with tests or record an explicit M5 deferral. Finally, accept the QA evidence and the release checklist against the signed build and hand the pointers to T13h. The emulator never closes a cell.
**Test cases:**
- TC-14.1 From-scratch run of the documented pipeline on this machine reaches the title screen and menu input on the adb device (the M4 definition).
- TC-14.2 Manual QA matrix complete and signed off (§8.3): every device cell has a dated report; three consecutive cold boots pass per device; every finding has a red-first regression test or a tracked open defect.
- TC-14.3 Suspend/resume and the 30-minute soak run clean on at least one physical device per session (hash evidence, no game frames in-repo).
- TC-14.4 Rumble/actuator audit resolved: implemented with tests and device observation, or a named M5 deferral with evidence.
- TC-14.5 Release checklist accepted against the signed build (clean clone, deps, notices, repository_check, CI, APK contents, no game/generated content, signing policy).
- TC-14.6 QA evidence set internally consistent and accepted as the M4/M5 evidence source (build SHA, device, counters, sign-off).
**Docs checkpoint:** `docs/QA.md`, dated `docs/qa/` session reports, the accepted release checklist in `docs/RELEASE.md`.
**Refs:** plan §8.3, hst-decomp (evidence labels and milestone ratchet), azazel6 (evidence labels); the device scripts and QA templates already in `docs/qa/`.
**Gotchas:** §7A.4 (public claims vs tree), §7E.1 (Android platform variance).

### T15 — Host save states (complete)
**Depends on:** T6 (kernel/HLE), T7 (GE state), T9 (Android shell)
**Work (done 2026-11-01, subtasks T15a–T15e):** own TLV snapshot container (magic GSNP v1) capturing RAM/CPU/clock, threads/kernel objects/timers, HLE state with derived rebuild; CLI `--snapshot-out/--snapshot-in`; Android Menu rows + JNI + intent; `docs/SNAPSHOT.md`; device QA record. Restored runs must match continuous runs report-for-report; independent restores byte-identical. PPSSPP used as behavioral reference only, never a link target.
**Docs checkpoint:** `docs/SNAPSHOT.md`, `docs/qa/` session report.
**Refs:** Yakumo (state shape), PPSSPP (behavioral oracle only).

### T16 — Widescreen rendering: display aspect and ultrawide (planned)
**Depends on:** T7, M4
**Work:** T16a projection widening + presentation; T16b fill-screen presentation (stretch, crop or fit). Research tasks complete; implementation pending.
**Research correction (2026-11-03):** 480x272 is 1.765:1, already almost 16:9 (16:9 is 483.6 px at 272 height), so a "16:9" host mode is a 0.7% change and visually identical to off for this title. The real target is the display aspect (19.5:9 → 604 px virtual, k = 1.26); 16:9 stays as a cap for other titles. T16b must land first: it owns the presentation policy enum, and T16a consumes it. Both tasks touch the same `fit_viewport`/scissor call sites, so they cannot run concurrently. Draw classification (`through` or a zero w-row projection term) is not carried to the renderer today and must be added to `DrawState`/`VulkanDrawState`; full-screen 2D effects must stretch, or fades show unfaded strips. Only display-buffer draws can widen — an offscreen 480-wide target is a counted gap. All `sceMpeg` entry points are a named gap, so FMV pillarboxing is unit-testable only; mark the device acceptance blocked, never failed.
**Test cases:** host projection/classification unit tests, presentation policy tests, snapshot identity rejection, mutation (widen everything breaks the ortho case) + device visual QA per mode, no gameplay-logic change (logic rate untouched — see T17).

### T17 — 60 fps support: pacing, interpolation and logic rate (planned)
**Depends on:** M4
**Work:** T17a frame pacing, interpolation and logic rate. Research complete. Battle-scene profiling feeds this task: 2026-11-02 simpleperf showed per-draw full-texture content hashing at ~83% CPU (fixed with a word-at-a-time hash, follow-up is a versioned texture cache); frame-rate evidence is gathered per scene on device, never from thumbnails.
**Test cases:** paced-frame timing tests + device fps evidence per scene (battle, event, menu).

### T18 — Text legibility: crisp font rendering at phone resolution (planned)
**Depends on:** T8, M4
**Work:** T18a sampling, HD atlas and replacement pipeline for dialog/UI text. Research complete; implementation pending.
**Test cases:** atlas regen round-trip + device legibility QA (no glyph collisions, ink-width asserts hold).

## 6. Milestones

| ID | Name | Entry criteria | Exit criteria | Docs checkpoint |
|---|---|---|---|---|
| **M0** | Repo live, policy enforced | — | T1+T2 done: `repository_check` green, deps pinned+audited, license machine runs | `LEGAL.md`, `SOURCE_PROVENANCE.md`, `PROVENANCE.md`, `AGENTS.md` |
| **M1** | Pipeline dry-run on synthetic fixtures | M0 | T4 done: synthetic fixtures trace-equal interpreter vs AOT on Linux; VFPU fuzz clean | `ARCHITECTURE.md`, `TESTING.md` |
| **M2** | Game analyzed, AOT generated | M1 + T3 intake gates pass on the local disc | T5 done: census gates pass, chunked TUs compile, dispatch-coverage report clean | `ANALYSIS.md`, `BRINGUP.md` opened |
| **M3** | Linux desktop bounded boot | M2 | T6+T7 done: bounded boot passes module init + first frame on Linux; GE fixtures pass on Vulkan | `HLE.md`, `RENDERER.md`, `SHADERS.md` |
| **M4** | **Title screen on adb device (first milestone)** | M3 + T9–T12 done | TC-14.1: documented pipeline reaches title screen + menu input (touch + gamepad) on device, and play extends into real battles with working in-game saves and host save states; **manual QA report accepted for every device in the matrix (§8.3), incl. one Adreno device (TC-14.2)** | `BUILDING.md`, `ANDROID.md`, `INPUT.md`, `PROGRESS.md`, `QA.md` + `qa/` session reports |
| **M5** | Hardening (post-milestone) | M4 | Audio complete (ATRAC/SAS), saves (memstick + states, PPSSPP-interchangeable), battle scenes at full speed on target handheld (T17), widescreen + crisp text (T16/T18), fast-forward (R2/R3/touch FF), texture replacement + text upscaling polished, driver picker, shader presets shipped; **manual QA accepted incl. custom-driver session, rumble, suspend/resume, 30-min soak (T14)** | `AUDIO.md`, `SAVEDATA.md`, `TEXTURE_REPLACEMENT.md`, `ANDROID_DRIVERS.md`, `qa/` reports |

Milestone discipline (hst-decomp ratchet): every milestone is *measured*, recorded in `data/milestones.json` with criterion + evidence, and a drop in any figure is **visible, never quietly lowered**. Additionally, **no device-facing milestone closes on automation alone** — the §8.3 QA matrix must be complete and signed off, and all red-first evidence (bug → reproducing test → fix) must be present for every defect found during the milestone.

## 7. Gotchas register

### A. IP / legal
1. `game.chd` already sits in the workdir — it must remain untracked **before the first commit**; T1 is not done until this is mechanically enforced.
2. Generated recomp C++ is a derivative of the copyrighted executable (PSPRecomp-family repos commit 108 MB of it — the anti-pattern we invert). Never committed, never in releases, never in screenshots of game output in-repo.
3. "No game data" claims must match the tree exactly (codepdbh claims it while shipping translated code; alexandercorp links ready-made game files on Google Drive). Documentation claims are evidence-checked in review.
4. EBOOT `expected_sha256` pins stay in gitignored `profiles/gungnir/local/`, not public TOML — pinning a dump hash publicly advertises a specific rip.
5. NID name tables: PPSSPP-derived data is GPL-tainted (usable here with attribution — we are GPL-3.0); psprism's committed `ppsspp_niddb.xml` shows what *unattributed* reuse looks like. Prefer BSD pspsdk-header generation.
6. Reject CC BY-NC-SA material entirely (PSPRecomp's CloudWorks clouds are the cautionary tale) — incompatible with GPL-3.0 and with any commercial future.
7. Third-party **binaries** are never committed (sal063's `SDL3.dll`); build from pinned source; LGPL pieces (FFmpeg/ATRAC) need notices + relink path.
8. `AdamFull/psprecore` is MIT, but its `nx-foundation` submodule has **no LICENSE** at `281fc91b`. Do not vendor, submodule or ship it; adapt psprecore code to std types instead. Revisit only if nx-foundation gains a GPL-3.0-compatible license and the provenance ledger is updated.

### B. Gungnir formats
1. DAR `stored_size == 0` means **stored uncompressed** (not empty) — the ptbr project zeroed 272 SFX missing this. Never trust `stored_size` alone.
2. DAR in-place rewrite rules: untouched entries keep their `data_off` (internal absolute references); recompressed blobs must fit the original slot (vary zlib params to squeeze); safest = true in-place + zero-fill tail.
3. Script `.bin` files contain absolute-address jumps and pointer tables; **never change file size** without remapping; floats impersonate pointers — only remap 4-aligned pointer-table runs, not "scanned pointers".
4. ISO 9660 directory records carry sizes both-endian (LE field + BE field +4) — patch both or the mount breaks (relevant if we ever repack).
5. Text slots in the EBOOT have real alignment padding; overruns land in relocated pointer words → "Bad Execution Address". The round-trip proof (original text through our writer = byte-identical image) is the writer's acceptance test.
6. Absolute-offset tooling is revision-fragile: every tool runs the identity gate (size + CRC32 `183F3EDC`) first; JP/EU offsets differ entirely.

### C. Static recompiler bug classes (the snuri00 10 + survey additions)
1. Import stub loops: `jal` to a stub must terminate codegen (`break`, not `continue`) — unbounded generation was a real bug class.
2. Data decoded as code: tolerate out-of-range VFPU register indices (read→0, write dropped) instead of build-failing on stripped binaries.
3. Full ISA coverage: one missing opcode (e.g. `ADDI`) is a hard runtime stop — audit every Allegrex encoding up front.
4. Scratchpad at `0x00010000` must be mapped from day one (16 KiB window) or games fault "outside RAM".
5. Function discovery: prologue-after-return heuristics AND HI16/LO16 pair seeding are both required (Gungnir's relocs use types 5/6); mine R_MIPS_32 for stored pointers; seed init/fini arrays; code can live **outside `.text`** (ctors in `.data`).
6. PRX tiers: `sceKernelLoadModule` loads code AOT never saw — per-module translation units + `unregister_functions(start,end)` + safe address reuse (Yakumo's overlay swap is the mature design).
7. VFPU exactness: `vrnd*` PRNG sequence, `vi2us/vi2s/vi2uc/vi2c`, `vt4444/vt5551/vt5650`, prefix quirks (decode-rotation + matrix prefixes bit wizardengineer twice); pin semantics with differential fuzz.
8. Backward local branches compiled as plain `goto` freeze the scheduler — counter-based forced dispatcher exit (~2048 transfers) keeps interrupts/heartbeat alive.
9. Dynamic code is a permanent runtime tier: interpreter + content-hashed block cache with memcmp self-modifying-code checks — not scaffolding to be removed later.
10. Delay-slot shapes differ subtly across engines (hoist+duplicate vs pending-target vs BIDS) — pick one model, test the foreign-owned-slot and branched-into-slot cases explicitly.
11. Jump tables: `jr $reg` ≠ return; recover via DATA xrefs where possible, fall back to runtime dispatch — never guess a table silently.

### D. Runtime / GE
1. Unknown NIDs that silently return 0 mask gaps until they explode in-game (azazel6 bans this) — loud named traps only.
2. Texture `bufw` handling (wizardengineer bug class) and CLUT-inclusive cache keys (sal063) are correctness-critical for Gungnir's 4bpp swizzled atlases.
3. Gungnir's PPSSPP log shows "render to area containing texture" — feedback-path invalidation is a named regression (TC-7.4), not a surprise.
4. Cooperative scheduling first (all surveyed repos converged); preemption is a designed-later phase — but never ship hidden stalls (yield counters + watchdogs).
5. 59.94 Hz pacing: frame = 16,683 µs; a wrong pace desyncs audio and TAS-like determinism tests.
6. Widescreen widening is host-side only: the guest still culls against its own camera, so geometry outside the unwidened frustum is never submitted and cannot be recovered without per-title camera hooks. Never claim "full widescreen"; count scenes that render into a 480-wide texture as a named gap.
7. PSP 480x272 is 1.765:1, already ~16:9 — the only meaningful widening target is the display aspect. Projection widening and presentation layout are separate settings: only the projection mode may enter the snapshot config identity, or every layout change invalidates save states. Vulkan scissor offsets must be non-negative (`VUID-vkCmdSetScissor-x-00595`), so crop with a negative origin must be clamped to the surface.
8. Fast-forward scales the **pacing hold**, never the guest clock: vblank cadence, VTimers, audio deadlines and every emulated delta stay at 59.94 Hz, so a save state taken while fast-forwarding restores normally. Scaling guest time instead would break determinism, audio sync and T15 snapshots. The speed is host state: it is not in the snapshot identity and not in the save. Audio is dropped (counted `muted_buffers`) rather than rate-converted, and a speed change rebases the hold instead of charging the new multiple for time already elapsed.

9. Frame interpolation is derived-only and lags one guest interval: in-betweens can only be built between a frame and its predecessor, so the display shows the previous interval's motion while the guest moves on. Never extrapolate past what the guest produced (the schedule holds the real frame instead), never interpolate through-mode/2D draws, and never let a snapshot load, reset or background resume inherit the previous history.

### E. Android platform
1. NDK Clang chokes on huge generated TUs — chunk generation (`FUNCS_PER_CHUNK`), force `-O2` even in debug APKs, per-unit `-O1` overrides, exclude generated corpus from LTO (codepdbh battle scars).
2. `MANAGE_EXTERNAL_STORAGE` is a Play-policy liability — SAF + `getExternalFilesDir()` only. Never commit `local.properties` or personal toolchain paths (alexandercorp did both).
3. CPU affinity/priority tuning: pin **after** Vulkan device creation (driver threads), with SELinux-safe `/sys`-read fallback (some devices block cpufreq reads).
4. AAudio: underruns at unbounded target rates (codepdbh: 240 Hz target broke, 60 Hz clean); fade + error-callback restart; resampler on the game-thread-fed FIFO.
5. Present path: do real swapchain presentation; codepdbh's per-frame GPU→CPU `ANativeWindow_lock` readback is the known perf trap.
6. Adreno driver zips: ABI mismatch and malformed archives must fail closed; drivers live in app-private storage and are **user's third-party binaries** — never committed, never bundled in APKs.
7. Lifecycle: background/foreground swapchain recreation is an explicit gap in the reference projects — it is a test case for us (TC-9.4), not an afterthought. Their answer is to leave the emulation core running behind the surface; ours stops it between dispatches and resumes in place, because a background guest only burns battery and would keep a bounded QA run's wall clock turning. A stop without a resume is never acceptable: `request_stop()` on background once shipped without one and froze the game on the last frame.
8. Touch overlay UX lineage (reVC/reIII) is GPL-tainted — reimplement behavior descriptions, record inspiration as prose only.
9. The runtime shares threads whose stacks it does not size — SDLActivity's Java `SDLThread` (about 1 MiB) and the session worker's `std::thread` — so one large local frame is a device-only, intermittent startup crash: the game-data identity gate kept a 1 MiB scratch buffer and overflowed the SDL thread at boot, with any small addition tipping it. Runtime targets compile with `-Werror=frame-larger-than=16384` (`cmake/BoundedStack.cmake`), `stack_space thread=<name>` logs the margin the thread really has, and `scripts/android/qa-startup.sh` boots ten times and fails on any crash.

### F. Gungnir-specific unknowns
1. `sceKernelLoadModule` usage (PPSSPP log) — which PRXs and when is discovered during T6; the overlay tier design must land before that, not after.
2. Engine script bytecode with absolute jumps is data, not code — but if any VM self-modifies or computes code pointers, the interpreter tier covers it (TC-5.4 counts those hits).
3. Dialog text = TPL font atlases with ink-width metrics (regenerating widths wrong causes collisions/clipping); UI text baked into TPL images needs image replacement, not font work — hence two-front text upscaling (T8).
4. TPL `bpp=5` CLUT layout is unmapped even in the ptbr tooling (grayscale only) — expected to matter little for text but must be decoded for correctness.
5. Audio decode quirk seen in PPSSPP logs (one reported error) — watch ATRAC edge cases in T12 soak tests.
6. Rumble: T11 lands **touch-control haptics** only (one short tick per button/d-pad touch-down; stick and drag/resize silent; system setting respected). Gungnir's actual sceRumble-class usage is unknown until bring-up; game/battle haptics and Gungnir mapping are T13/M5.
7. PPSSPP is the behavioral oracle (Perfect→Playable) — used for **diffing only, never linked or vendored** (sp00nznet's rule).

## 8. Cross-cutting test strategy

### 8.1 Red-first discipline (no regressions)

Every behavioral change follows the same loop, without exception:

1. **Red first.** The test (or fixture, gate, trace-assertion) lands *before* the implementation and is observed failing **for the expected reason** — a red run that fails for an unrelated error doesn't count. Commit the red test alongside the implementation it drives.
2. **Reproductions before fixes.** A bug fix starts with a test that reproduces the bug against current code (red), then the fix turns it green. This applies to bring-up bugs too: every `docs/BRINGUP.md` root-cause entry ends with a regression test name.
3. **No task closes red.** A task's test cases (TC-x.y) are its acceptance criteria; the task is done when they are green, and any test that must be disabled gets an itemized, time-boxed TODO — never a silent skip.
4. **Mutation checks gate semantic changes** (hst-decomp rule): for codegen/HLE changes, deliberately break one instruction/NID semantic and confirm the suite dies *for that reason* — mutations only count as evidence when they kill the gate correctly.
5. **Where red-first is not applicable:** pure research/spikes (e.g. Ghidra census tuning) and one-shot intake tooling (single known input) — but anything that touches runtime semantics, codegen, formats (DAR/TPL writers), or HLE behavior is red-first.

### 8.2 Oracle hierarchy & fixtures

1. **Oracle hierarchy:** our interpreter (in-repo truth) → synthetic golden traces (hand-computed expectations) → PPSSPP headless diff (optional, test-only) → on-device manual QA (see 8.3). Never link an emulator into the product.
2. **Synthetic-first:** every subsystem (decode, codegen, HLE, GE, TPL, DAR, SFO, input) has fixtures of **our own authorship** committed to `tests/fixtures/`; private game tests exist locally and `exit 77` in CI.
3. **Round-trip invariants:** every writer (DAR repack, TPL re-encode, font atlas repack, PARAM.SFO) must reproduce its input byte-identically before it may write anything new.
4. **Bounded runs everywhere** (azazel6): dispatch + wall-clock limits on any guest execution; synthetic runs are never reported as game runs; evidence labels (VERIFIED/HYPOTHESIS) in `docs/BRINGUP.md`.
5. **CI fail-closed:** tests needing game data skip with exit 77 (never pass silently); `repository_check` + secret scan + license gate are merge-blocking.

### 8.3 Manual QA on adb devices (mandatory)

Automated tests never close a device-facing milestone. Manual QA is a structured, recorded activity:

- **QA matrix per device-facing milestone (M4/M5):** at minimum one **Adreno** device (for driver-loader validation) and one **non-Adreno or handheld** device (Ayn/Ayaneo-class where possible) + the x86_64 emulator. Each cell records: device model, Android version, GPU/driver (system or custom zip + version), build ID (git SHA), date, tester.
- **Emulator vs device:** emulators are fine during initial feature bring-up and fast iteration, but never close a device-facing feature sprint. Physical-device manual QA is required at sprint end; emulator evidence supplements it. Start the reproducible x86_64 emulator with `scripts/android/emulator.sh` (AVD `gungnir`, port 5554, headless SwiftShader, `-no-snapshot -wipe-data -no-audio -no-boot-anim -netdelay none -netspeed full`); `-wipe-data` resets the AVD, so re-push user data.
- **Standard QA script** (`docs/QA.md`, kept current): cold boot to title; menu navigation via touch; menu navigation via gamepad (paired + built-in handheld controller); overlay hide-on-pad; suspend/resume (background/foreground) in-place guest resume with progress intact; rotation ignored; save/load if enabled; touch-control haptic tick on the phone and no-op on the emulator; battle/game rumble if enabled later; audio smoke; fast-forward by R2 hold, R3 press and the touch FF toggle (progress per wall second, audio drops without xruns, the latch survives suspend/resume); 30-minute soak for crashes/thermal. T14 adds the Gungnir-specific script: title → menu → first gameplay transition.
- **Red-first applies to QA findings too:** a device bug becomes a reproducible case (steps, build, device) and, wherever host-testable, a red test before the fix.
- **Evidence:** QA reports live in `docs/qa/` (markdown, one file per session, dated) with **counts and pass/fail only** — no game screenshots in-repo; screenshot hashes recorded instead (images stay in gitignored `build/qa/`).
- **Sign-off:** M4 and M5 close only with an accepted QA report per device in the matrix. "Works on my machine once" is not evidence; "CI is green" is not evidence; three consecutive cold boots on each device are.

## 9. Documentation checkpoints (summary)

| When | Documents |
|---|---|
| Before any code (T1) | `AGENTS.md`, `docs/LEGAL.md`, `docs/SOURCE_PROVENANCE.md` |
| With deps (T2) | `docs/PROVENANCE.md` (per-subtree ledger) |
| With intake (T3) | `docs/INTAKE.md` |
| With core (T4) | `docs/ARCHITECTURE.md`, `docs/TESTING.md` |
| With analysis (T5) | `docs/ANALYSIS.md`, `docs/BRINGUP.md` (living journal) |
| With HLE (T6) | `docs/HLE.md` |
| With renderer (T7) | `docs/RENDERER.md`, `docs/SHADERS.md` |
| With assets (T8) | `docs/formats/{TPL,DAR}.md`, `docs/TEXTURE_REPLACEMENT.md` |
| With Android (T9–T11) | `docs/ANDROID.md`, `docs/ANDROID_DRIVERS.md`, `docs/INPUT.md` |
| With audio/saves (T12) | `docs/AUDIO.md`, `docs/SAVEDATA.md` |
| At milestone (T13/T14) | `docs/BUILDING.md`, `docs/QA.md` + `docs/qa/<date>-<device>.md` session reports, `docs/PROGRESS.md`, `docs/RELEASE.md` |

Every document obeys §0: format facts and RE addresses yes; game content, dump hashes, and screenshots no.

## 10. Execution order (dependency graph)

```
T1 ──► T2 ──► T4 ──┬──► T5 ──► T6 ──┬──► T12 ──┐
     └─► T3 ──────┘         │       │          ├──► T13 (M4: title screen on device)
              └─────────────┴──► T7 ─┼──► T8 ──┤
                                     └──► T9 ──► T10 ──┤
                                            └──► T11 ──┘
T13 ──► T14 (M4 QA close-out) ──► T15 (host save states, done) ──► M5: T16 (widescreen) + T17 (60 fps) + T18 (text)
```

Start: **T1 (scaffolding + IP guardrails)** — nothing else may begin until `repository_check` is green and `game.chd` is mechanically untrackable.

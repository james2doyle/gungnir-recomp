# ANALYSIS — Gungnir static analysis and AOT corpus (T5)

Input is the local decrypted module produced by `tools/intake` (`build/game/`):
the main game module `9th` (disc path `PSP_GAME/SYSDIR/EBOOT.BIN`). The
`updater` module from `SYSDIR/UPDATE/EBOOT.BIN` is deliberately excluded: it
is the firmware-updater payload, not game code, and is never loaded by the
title. That exclusion is counted here, not silent.

`profiles/gungnir/config.toml` is the facts-vs-choices record. Facts (disc id,
module name/path, load base) are re-verified against the intake manifest every
run and abort with `FACTS` on mismatch. Choices (unit span, census bounds,
policy) are ours. Dump hashes, entry addresses, and census actuals are written
only to the gitignored `profiles/gungnir/local/profile_pins.json`.

## Discovery seeds (VERIFIED, this dump)

The analyzer is the vendored MIT program analysis (`third_party/yakumo`),
extended with recorded local patches (module-info fallback, HI16/LO16 pairing,
prologue-after-return seeding, and an overlay-safety fix in
`unregister_functions`). Seed sources on the main module:

| Source | Seeds | What it catches |
|---|---:|---|
| `direct_jal_target` | 3,616 | ordinary calls |
| `relocated_r_mips32_code_pointer` | 1,594 | stored function pointers in tables/vtables (plan gotcha C.5) |
| `prologue_after_return` | 545 | `jr $ra` followed by a new stack-frame prologue |
| `hi16_lo16_pair` | 282 | relocated `lui`+`addiu`/`ori` pointer materialization, including pairs split across block boundaries |
| `materialized_code_pointer` | 22 | LUI+ADDIU/ORI without relocations |
| `relocated_data_code_pointer` | 2 | relocated words in data segments |
| `elf_entry` | 1 | module entry |
| **total** | **6,062** | |

Init/fini arrays and constructors in `.data` are covered by the same
`R_MIPS_32` mining: their entries are relocated code pointers stored in
non-executable segments, so no separate init/fini scan is needed on this dump.

`hi16_lo16_pair` and `prologue_after_return` are local-patch evidence:
relocation types 5/6 are paired by register in address order, and adjacent
functions with no call or pointer are found by return/prologue shape. Before the patch the source was absent (red); the
first implementation produced zero attributed seeds because `add_seed` keeps
the first source and the materialized-pointer scan had already claimed the
targets — fixed by ordering the authoritative relocation pass first
(`docs/BRINGUP.md`, entry 2).

No functions were truncated; 33 unsupported-instruction occurrences are
recorded (they become loud runtime stops under the profile policy, never
silent no-ops).

## Executable coverage (VERIFIED)

The linked image has one RWX `PT_LOAD`, but 24 `SHF_EXECINSTR` sections are
present and backed by it. Analysis restricts itself to those sections, exactly
as codegen does:

| Figure | Words | Share |
|---|---:|---:|
| Executable words (24 X sections) | 424,373 | 100% |
| Reached as instructions by analysis | 359,429 | 84.7% |
| Not reached; emitted as explicit data/trap entries | 64,944 | 15.3% |
| Accounted (analyzed + counted data/trap) | 424,373 | **100%** |

The prologue-after-return pass added five functions (6,057 -> 6,062) and 144
newly analyzed instructions; all other discovery sources were unchanged in
address coverage. TC-5.1 asserts the accounting identity and census bounds,
not an eyeballed number. Codegen independently asserts that the number of registered
entry points equals 424,373, so every executable word is dispatchable. A
computed jump into code the CFG never reached does not stop the runtime: it
lands on a generated entry, which executes the decoded instruction or traps
loudly if the word is data (`unsupported_instruction=stop`).

Optional Ghidra DATA-xref jump-table recovery is not implemented in T5. The
runtime dispatch fallback above is the designed fallback in the plan; a Ghidra
export can be added later without changing the corpus contract.

## Import inventory (VERIFIED)

The intake manifest lists 215 imports for `9th` across 23 libraries. All 215
stub addresses fall inside executable ranges (`0` unmapped), and codegen emits
215 HLE wrappers. Without the module-info fallback the stripped section-name
table made the analyzer report 0 imports and codegen emit 0 wrappers: a call
to a stub would have returned silently through `jr $ra`. The fallback validates
candidate module headers functionally with `scan_imports()`; wrappers now call
`invoke_import_cached()`, which stops with `Missing HLE import` until T6 binds
the real HLE surface.

Largest libraries: ThreadManForUser 47, sceSasCore 27, sceMpeg 24,
IoFileMgrForUser 18, sceUtility 15, sceGe_user 11, sceAudio 9.

## Corpus (VERIFIED)

`gungnir_profile.py codegen` partitions the executable address space by
16 KiB spans: 104 translation units, 424,373 registered entries, 215 import
wrappers. Repeated generation rewrites zero units and leaves every generated
source byte-identical (TC-5.2). All units compile; the test suite compiles the
whole corpus with `-O0` and links the coverage harness.

Generated C++ is build output under `build/`; it is never tracked
(`repository_check` + TC-4.3 enforce this).

## Dispatch coverage (TC-5.4)

`tests/core/game_coverage.cpp` links the generated corpus, runs from the
module entry with `Runtime::run` bounded by dispatch count, and classifies
every outer dispatch:

- **translated** — `Runtime` found a registered function and ran it;
- **interpreter fallback** — no function claimed the PC, the permanent
  interpreter tier ran, and the PC is listed explicitly.

Chaining is disabled by the dispatch hooks, so each classification is one
real outer dispatch. The negative control drops the entry unit
(`unregister_functions`) and requires a nonzero, listed fallback count. It
caught a real overlay-safety bug: the fast unit table was consulted before the
per-PC map, so a fully removed unit stayed dispatchable. Local patch 6 clears
that pointer; the regression is the TC-5.4 negative control. The
first real run stops at the first import with `Missing HLE import …` (T6 work),
which is the intended bounded-run endpoint for T5; synthetic runs are not
reported as game runs.

## Reproduction

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 tools/profile/gungnir_profile.py analyze \
    --config profiles/gungnir/config.toml \
    --manifest build/game/manifest.json --out build/profile/analysis
python3 tools/profile/gungnir_profile.py codegen \
    --config profiles/gungnir/config.toml \
    --analysis build/profile/analysis/analysis.json --out build/generated
python3 -m unittest tests.test_profile -v
```

The suite skips when the local disc is absent (private input); it never
passes silently on missing data.

## Status (2026-10-30)

M2 closed. Evidence: `tests/test_profile.py` 16/16 green (TC-5.1–TC-5.4),
full suite 49/49 green, `tools/repository_check.py` PASS. Generated corpus and
analysis manifests live under `build/`; only the config, tools, tests, and docs
are tracked. The next consumer is T6 (HLE kernel): the bounded run stops at
the first import (`Missing HLE import`), which is the intended hand-off.

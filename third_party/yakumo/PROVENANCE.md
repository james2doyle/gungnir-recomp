# PROVENANCE — third_party/yakumo

| Field | Value |
|---|---|
| Upstream | https://github.com/TeamGDB/Yakumo |
| Commit | `b61852aef10870400320b96d92a14136ac17b051` |
| License | MIT (repo root `LICENSE`, copied here verbatim) |
| Imported | 2026-10-30 (framework core + tooling added with T4) |
| Disposition | **byte-identical except one recorded local patch** (see “Local patches”) |

## Files

### Readers and platform helpers (T3)

| Upstream path | Here |
|---|---|
| `src/elf32.cpp` | `src/elf32.cpp` |
| `include/psprecomp/elf32.hpp` | `include/psprecomp/elf32.hpp` |
| `include/psprecomp/common.hpp` | `include/psprecomp/common.hpp` |
| `include/psprecomp/guest_memory.hpp` | `include/psprecomp/guest_memory.hpp` |
| `src/guest_memory.cpp` | `src/guest_memory.cpp` |
| `profiles/mhp3rd/host/kernel/iso_image.cpp` | `src/iso_image.cpp` |
| `profiles/mhp3rd/host/kernel/iso_image.hpp` | `src/iso_image.hpp` |
| `profiles/mhp3rd/host/platform/utf8_path.cpp` | `src/utf8_path.cpp` |
| `profiles/mhp3rd/host/platform/utf8_path.hpp` | `include/platform/utf8_path.hpp` |
| `profiles/mhp3rd/host/save_data/param_sfo.cpp` | `src/param_sfo.cpp` |
| `profiles/mhp3rd/host/save_data/param_sfo.hpp` | `include/save_data/param_sfo.hpp` |

### Framework core + tooling (T4)

| Upstream path | Here |
|---|---|
| `include/psprecomp/allegrex_context.hpp` | same |
| `include/psprecomp/codegen_policy.hpp` | same |
| `include/psprecomp/decoder.hpp` | same |
| `include/psprecomp/deflate.hpp` | same |
| `include/psprecomp/interpreter.hpp` | same |
| `include/psprecomp/nid_registry.hpp` | same |
| `include/psprecomp/program_analysis.hpp` | same |
| `include/psprecomp/runtime.hpp` | same |
| `include/psprecomp/sha256.hpp` | same |
| `src/decoder.cpp` | same (one local patch, below) |
| `src/deflate.cpp` | same |
| `src/interpreter.cpp` | same |
| `src/nid_registry.cpp` | same |
| `src/program_analysis.cpp` | same |
| `src/runtime.cpp` | same |
| `src/sha256.cpp` | same |
| `tools/analyzer_main.cpp` | `tools/analyzer_main.cpp` |
| `tools/codegen_main.cpp` | `tools/codegen_main.cpp` |
| `tools/dump_function.cpp` | `tools/dump_function.cpp` |

Not imported: `configs/nids.csv` (upstream does not document its provenance;
the plan requires NID data generated from BSD pspsdk headers with a pinned
checksum — T6 decision, deliberately not copied). `profiles/mhp3rd/**`
everything else: title-specific, out of scope.

## Local patches

1. `src/decoder.cpp` — **ADDI (opcode 0x08) mapping added** (`kind Addiu`,
   mnemonic `addi`). Upstream 0x08 is unhandled, so any ADDI word stopped both
   tiers with "unknown not interpreted" (plan gotcha C.3, found by the T4
   fixture harness; regression: `tests/core/fixtures/addi_opcode.mips`).
   Semantics: no overflow trap is modelled — Allegrex user code has no
   observable overflow exception in this runtime, so ADDI executes with ADDIU
   behavior. Documented divergence; review if a guest ever depends on traps.
   Upstream issue to file when reporting to Yakumo; drop on re-sync if fixed
   upstream with equivalent semantics.
2. `include/psprecomp/guest_memory.hpp` + `src/guest_memory.cpp` — **16 KiB
   Allegrex scratchpad mapped at 0x00010000** (new `Region::Scratchpad`,
   `contains`/`resolve`/`region_bytes` cases). Upstream maps only RAM and
   VRAM; guest code touching the scratchpad faulted "outside PSP RAM/EDRAM"
   (plan gotcha C.4). Regression: `tests/core/fixtures/scratchpad_basic.mips`.
   The AOT inline fast path already declines the scratchpad window and falls
   through to the slow path, so no generated-code change was needed.

## Emitted-output notes

`tools/codegen_main.cpp` writes generated C++ into the caller's output path —
that output is build output, never committed (plan §7A.2).

## Scope rationale

Selected per the adoption decision (plan §1): MIT framework code from the
PSPRecomp lineage, audited clean of game content — `iso_image.cpp` and
`param_sfo.cpp` carried only a `mhp3rd` namespace name, zero MHP3rd policy
or addresses (verified by search before import). `iso_image` is specifically
the ISO reader seam azazel6's reuse review identified as a demonstrated
reusable component.

`guest_memory.cpp` requires three embedder-provided diagnostics it declares
extern (`runtime_thread_uid`, `runtime_thread_name`, `runtime_dispatch_pc` —
upstream defined in `runtime.cpp`, which is outside this subset's scope).
Hosting binaries must define them; the intake tool defines inert versions
feeding env-gated logging paths only. This is a declared seam of the vendored
file, not a local patch of it (byte-identical disposition stands).

## Known gap (ours to handle outside this subtree)

`elf32.cpp` recognizes only relocation section type `0x700000A0`
(`kSectionTypePspRel`). Gungnir's decrypted EBOOT uses `0x70000020` with the
same 8-byte `[r_offset:u32, r_info:u32]` entries (types 2/5/6). Callers in
`tools/intake/` therefore normalize/accept both section types rather than
patching this vendored code. If upstream later supports `0x70000020`,
re-sync and drop the shim.

## Update procedure

Re-vendoring = copy the listed paths at a new pinned commit, re-run the
game-content audit (grep for title/serial/address constants), diff against
this tree, update the commit row above, and record the delta in the
importing task's notes.

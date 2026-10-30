# PROVENANCE — third_party/yakumo

| Field | Value |
|---|---|
| Upstream | https://github.com/TeamGDB/Yakumo |
| Commit | `b61852aef10870400320b96d92a14136ac17b051` |
| License | MIT (repo root `LICENSE`, copied here verbatim) |
| Imported | 2026-10-30 |
| Disposition | **byte-identical** (no local modifications; `mhp3rd` namespace names kept as-is) |

## Files

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

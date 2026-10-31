# PROVENANCE — third-party dependency ledger

Per-subtree record of everything vendored or adopted. Policy authority:
`AGENTS.md` (hard limits #2–#4); import process:
[`docs/SOURCE_PROVENANCE.md`](SOURCE_PROVENANCE.md). Machine-readable pins
live in [`dependencies.json`](../dependencies.json) (name, upstream, exact
commit, license, in-tree license file, **content digest**); every entry is
verified by `tools/deps.py verify` (TC-2.1) and by
`tools/package_notices.py` (coverage TC-2.2, license gate TC-2.3).

## Ledger

| Subtree | Upstream | Commit | License | Disposition | Notes |
|---|---|---|---|---|---|
| `third_party/pspsdk-nids` | pspdev/pspsdk | `c3da4314` | BSD-3-Clause (in-tree `LICENSE`) | generated NID facts (`configs/nids.csv`); no upstream source vendored | `third_party/pspsdk-nids/PROVENANCE.md` |
| `third_party/yakumo` | TeamGDB/Yakumo | `b61852af` (full pin in dependencies.json) | MIT (in-tree `LICENSE`) | subset (readers + framework core + CLI tooling), **7 recorded local patches**; kernel/HLE adapted outside the tree | per-file mapping, patch list, adapted-file list, game-content audit: `third_party/yakumo/PROVENANCE.md` |
| `third_party/atrac3p` | Jstar269/nakagawa-recomp (FFmpeg n4.4 subset) | `1117d635` (FFmpeg tag/blob pins inside) | LGPL-2.1-or-later (in-tree `LICENSE.LGPLv2.1.txt`) | decoder subset byte-identical; one additive `extern "C"` guard in the authored API header | FFmpeg blob manifest, local deltas, boundaries: `third_party/atrac3p/PROVENANCE.md` |
| `third_party/libadrenotools` | bylaws/libadrenotools | `8fae8ce2` (full pin in dependencies.json) | BSD-2-Clause (in-tree `LICENSE`) | adopted, **fetch-only**; built from source for Android arm64, no source or binary committed | ICD loading, linker-namespace hooks; submodule composition and constraints: `third_party/libadrenotools/PROVENANCE.md` |
| `third_party/liblinkernsbypass` | bylaws/liblinkernsbypass | `aa397589` (full pin in dependencies.json) | BSD-2-Clause (in-tree `LICENSE`) | adopted, **fetch-only**; transitive submodule of libadrenotools, no source or binary committed | private linker namespace + soname patching: `third_party/liblinkernsbypass/PROVENANCE.md` |

## Rules this ledger enforces

1. **Pin + digest**: every entry carries the upstream commit and a sha256
   over its vendored content (canonical form in `tools/deps.py`). A tampered
   GitHub or local tree fails `deps.py verify` by name — vendoring is
   verified, not trusted.
2. **Coverage**: nothing under `third_party/` may exist without a declaring
   entry in `dependencies.json` (`UNLISTED_THIRD_PARTY_FILE` otherwise).
   This is the CI gate against unattributed blobs (the psprism
   `ppsspp_niddb.xml` failure mode).
3. **License gate**: only GPL-3.0-or-later-compatible licenses pass
   (MIT / BSD / ISC / Apache-2.0 / LGPL / GPL families per SPDX list in
   `tools/package_notices.py`). CC BY-NC-SA and unattributed code are
   rejected by name.
4. **In-tree licenses**: each vendored tree carries its upstream LICENSE
   verbatim; `THIRD_PARTY_NOTICES.txt` is generated from it
   (`tools/package_notices.py generate`, output committed, header says
   do-not-hand-edit). The plan's separate `THIRD_PARTY_LICENSES/` directory
   is deliberately superseded by this rule — one license source of truth,
   no duplicated copies to drift apart.
5. **Vendored ≠ patched**: vendored files stay byte-identical; caller-side
   needs are satisfied by shims *outside* the subtree (recorded per-subtree
   below). If upstream gains the needed capability, re-sync at the same pin
   and drop the shim.

## Per-subtree shims and seams (current)

- `third_party/yakumo`: `guest_memory.cpp` expects three embedder-provided
  diagnostics (`runtime_thread_uid`, `runtime_thread_name`,
  `runtime_dispatch_pc` — upstream defines them in its `runtime.cpp`, which
  is outside the adopted subset). Hosting binaries define them; the intake
  tool defines inert versions feeding env-gated logging only. Intake also
  adds a content-anchored module-info fallback for fully stripped ELFs and
  accepts both PSP relocation section flavors (`0x700000A0`, `0x70000020`) —
  details in `third_party/yakumo/PROVENANCE.md`.

## Update procedure

1. `gh repo clone <upstream>` at the new commit; re-run the game-content
   audit (search for title names/serials/known RE constants).
2. Replace the vendored paths, diff against the old tree, and update the
   **commit** and **digest** rows in `dependencies.json` (recompute with
   the `tools/deps.py` canonical rule).
3. Update the subtree `PROVENANCE.md` mapping table and this ledger.
4. `python3 tools/deps.py verify && python3 tools/package_notices.py generate`
   and land both outputs with the change.

SDL3 (T9) is fetch-based, not vendored: `cmake/Deps.cmake` materialises the
pinned `release-3.4.18` source tarball (SHA-256 in `dependencies.json`) into
the machine-local `build/android-deps` tree, with fetch-or-reuse and an
offline mode. Desktop builds may use a system SDL3 that satisfies the pin; the
Android Gradle build compiles SDL's Java layer from the same pinned tree.
`tools/deps.py` verifies the archive when it is materialized and reports the
pin as not materialized otherwise — never a silent skip. Only the license and
provenance note live under `third_party/sdl3/`.

libadrenotools and its `liblinkernsbypass` submodule (T10) follow the same
fetch-only rule: each commit-archive SHA-256 is pinned in `dependencies.json`,
and the Android build verifies both archives before unpacking them under
`build/android-deps`. GitHub commit archives exclude submodule content, so
liblinkernsbypass is a separate pinned entry that is unpacked into
`libadrenotools/lib/linkernsbypass/`. No upstream source, object, or driver
binary is committed; only licenses and provenance notes live under
`third_party/libadrenotools/` and `third_party/liblinkernsbypass/`.

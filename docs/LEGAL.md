# LEGAL — the IP charter

This document is the legal policy of the **gungnir-recomp** project. It is
normative: contributions that violate it are rejected, and the rules are
enforced mechanically (see *Enforcement* below). The summary in `AGENTS.md`
governs day-to-day work; this file is the full charter.

## 1. What this project is

An original software project: tooling and a runtime that **statically
recompiles** Allegrex (PSP) machine code to native C++, plus an HLE PSP
system-software layer, a Vulkan renderer, and an Android application shell.

*Gungnir: Inferno of the Demon Lance and the War of Heroes* (PSP, USA,
ULUS-10592) is the target title. Its copyright belongs to Sting Co., Ltd. /
Atlus U.S.A., Inc. **This project is not affiliated with, endorsed by, or
licensed by Sting or Atlus.** The project's output runs only with a copy of
the game that the user lawfully possesses.

## 2. The absolute rule: no proprietary material in the repository

The following must **never** be committed, pushed, attached to issues/PRs,
embedded in docs, or included in releases:

1. **Disc images and dumps** in any form: `.iso`, `.chd`, `.cso`, `.zso`,
   `.pbp`, and fragments thereof.
2. **Game executables and modules** — `EBOOT.BIN`, `BOOT.BIN`, `*.elf`,
   `*.prx` — in any state: encrypted (`~PSP`), decrypted, decrypted-and-
   relocated, partially decrypted.
3. **Game assets**: DAR archive contents, TPL textures, VAG/AT3/ATRAC audio,
   PMF/PSMF movies, PGF font data, script `.bin` files, PARAM.SFO from the
   disc, and any extracted/converted form of these.
4. **Game text** — the original Japanese/English strings, and translations
   derived from them (fan or official).
5. **Game depictions** — screenshots, photos of a screen, video capture,
   audio capture, in any project-controlled location including docs, tests,
   and the website.
6. **Generated recompilation output** — AOT-translated C/C++ units, analysis
   reports derived from game code, dispatch tables, and any other artifact
   mechanically derived from the copyrighted executable. This includes
   output committed "for convenience" to profiles or branches. It is a
   derivative work of the game; it exists only in the user's local `build/`
   tree and is regenerated from their own disc.
7. **Sony firmware and its data**: IPL, flash0 modules, PGF system fonts
   (`jpn0.pgf`, `ltn0.pgf`, …), and any dump of them.
8. **Keys and credentials**: KIRK key vaults, DNAS keys, `~PSP`/PGD key sets,
   key-tag tables that enable decryption, `.pem`/keystores, and vendor
   secrets. Not in code, not in tests, not in docs (format placeholders that
   are clearly fake text are permitted in docs only).
9. **Third-party proprietary binaries**: GPU driver packages (Qualcomm
   drivers, Mesa Turnip zips are *user-supplied* at runtime), prebuilt SDK
   blobs, `.so`/`.dll`/`.exe`/`.jar`/`.zip` of any kind. Dependencies are
   built from pinned source (see `dependencies.json`).
10. **Other games' material**: translation corpora, code maps, or data from
    any other commercial game (e.g. GTA VCS generated corpora) — even under
    permissive license headers, translated game code is a derivative work of
    *that* game.

## 3. What the project *may* contain

- Original source code authored for this project (GPL-3.0-or-later).
- Third-party open-source code with GPL-3.0-compatible licenses, pinned,
  attributed, with license texts preserved (`docs/SOURCE_PROVENANCE.md`).
- **Game facts that are not creative expression**: the redump-published disc
  identity (size 262,766,592 bytes, CRC32 `183F3EDC`), file-format field
  layouts (DAR/TPL/SFO structures), PSP NID values, guest addresses, record
  sizes, and similar reverse-engineering facts. Facts are not copyrightable;
  game *content* is.
- Synthetic fixtures of our own authorship (committed as text sources).
- Documentation describing formats and behavior abstractly.

## 4. User-supplied inputs (the bring-your-own-game model)

The user provides, locally and privately:

- Their own legally obtained disc image (`game.chd`/ISO). Identity is
  verified (size + CRC32) and everything derived from it stays under
  gitignored `build/` and `profiles/*/local/`.
- **Decryption is performed out-of-process** by tools the user supplies or
  installs themselves (e.g. `pspdecrypt`, PPSSPP devtools). This project
  ships no decryption implementation and no keys. Our pipeline consumes the
  user's decrypted ELF and validates its shape; it never redistributes it.
- Optional: GPU driver `.zip` packages (Adreno) extracted by the app into
  app-private storage on the user's device. Never bundled, never committed.
- Optional: fonts supplied by the user for replacement rendering.

The project's scripts must never copy user-supplied game material anywhere
trackable. All such paths are gitignored **and** blocked by
`tools/repository_check.py` **and** excluded from CI.

## 5. Dump identities stay private

Hashes of the game's binaries (e.g. the decrypted EBOOT SHA-256) pin *which
dump* a user has and are therefore kept in gitignored
`profiles/gungnir/local/`. Publishing them would advertise a specific rip.
What is published is only the redump-recorded disc identity (§3), which is
already a matter of public record.

## 6. Licensing of this project's own work

Project-original code: **GPL-3.0-or-later** (see `LICENSE`). By contributing,
you agree your contribution is licensed the same way and that it contains no
material prohibited by §2. Do not paste or adapt code from a project whose
license is incompatible with GPL-3.0 (notably CC BY-NC-SA or "all rights
reserved" sources). Recording behavioral *inspiration* from such projects as
prose is fine; importing their code is not.

## 7. Enforcement (structural, not aspirational)

1. `.gitignore` blocks game material class-by-class, with rationale comments.
2. `tools/repository_check.py` audits every staged/tracked/visible file:
   forbidden extensions, magic bytes (ELF, `~PSP`, CHD, PGF, PBP, PE, ISO
   9660), blob-size cap, secret patterns, and **ignore-rule probes** (if
   someone weakens `.gitignore`, the gate goes red). Tested by
   `tests/test_repository_check.py`; run in CI and before every commit.
3. CI runs only on synthetic fixtures — never on game data (private-input
   tests skip fail-closed). Secret scanning on full history.
4. Documentation claims are evidence-checked in review: if the README says
   "no game data included", the tree must prove it.

## 8. Consequences

A contribution containing anything from §2 is reverted and its history
scrubbed. Repeat or knowing violations end in a ban from contributing. If
you find proprietary material in the repository or its history, open a
private security advisory (see `AGENTS.md`) rather than an issue.

## 9. Contact

Project maintainers via the repository's GitHub page. Trademark note: PSP and
PlayStation are trademarks of Sony Interactive Entertainment Inc.; all other
trademarks belong to their respective owners; use here is nominative.

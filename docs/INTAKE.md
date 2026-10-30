# Intake — supplying your own disc (T3)

Intake turns your own `game.iso` into a verified local module tree under
`build/game/`-style output. **Disc input is ISO only** — a `.chd` is never
parsed and is refused outright (convert it once yourself first —
[CHDStudio](https://github.com/purelogiccode/CHDStudio) or
`chdman extractcd`; see §1). It never decrypts in-repo, and
never writes outside the output directory. Every gate fails closed with a
**named error** — no silent skips.

## 0. Inputs (what you own)

| File | Role |
|---|---|
| Your disc image | source of truth: a PSP UMD **ISO** (converted from your CHD, §1) |
| Decrypt tool | user-supplied, runs out-of-process (see §3) |

The repo never receives any of these. Intake reads them, gates them, and
records facts (paths + hashes) only under `build/`.

## 1. Convert CHD → ISO (once, user-side)

Disc input is **ISO only**. Convert your CHD yourself with one of:

- [CHDStudio](https://github.com/purelogiccode/CHDStudio) (GUI, recommended)
- `chdman extractcd` (MAME CLI)

The identity gate verifies — does not trust — whatever produced the ISO:
size **262,766,592** bytes and CRC32 **`183F3EDC`** (redump-recorded disc
facts for Gungnir USA / ULUS-10592). A `.chd` handed to intake is refused
outright with `CHD_NOT_SUPPORTED`; a wrong image is refused with
`IDENTITY_MISMATCH`.

```sh
python3 tools/intake/intake.py verify --iso game.iso --expect-size 262766592 --expect-crc 183F3EDC
# defaults already encode the redump facts for this title:
python3 tools/intake/intake.py verify --iso game.iso
```

## 2. Extract + manifest

```sh
python3 tools/intake/intake.py run --iso game.iso --out build/game
```

Steps performed (in order), all outputs confined to `--out`:

1. **Identity gate** (`IDENTITY_MISMATCH` on size/CRC mismatch).
2. **ISO9660 extraction** of `PSP_GAME/` into `out/files/`
   (`ISO_INVALID` if the image does not parse).
3. **PARAM.SFO** validation: `DISC_ID` must resolve to ULUS-10592
   (`DISC_ID_MISMATCH`, `SFO_MISSING`).
4. **Module inventory**: every SYSDIR executable (`EBOOT.BIN`, `BOOT.BIN`,
   `*.prx`) becomes a manifest module with SHA-256, segments, relocation
   counts and types, and the NID **import inventory**.
5. `manifest.json` written to the output root; hashes are computed from the
   extracted bytes (`EXTRACT_SIZE_MISMATCH` if the tool and the orchestrator
   ever disagree).

DOM-side facts recorded by the manifest (from actual local runs:

- `SYSDIR/EBOOT.BIN` is `~PSP`-encrypted; `SYSDIR/UPDATE/EBOOT.BIN` too;
- `SYSDIR/BOOT.BIN` on this disc is **not** a plaintext ELF (neither
  `~PSP` nor ELF magic) — it is recorded in the manifest as
  `UNRECOGNIZED_SHAPE`, a counted gap, and never claimed as validated;
- the decrypted main module (`e_type 0xFFA0`) uses PSP reloc section type
  **`0x700000A0`** (49,396 entries; the plan's predicted `0x70000002`0 did
  not occur — both section types are accepted and the actual one recorded;
- modules differ in address-regime: the updater keeps runtime-absolute
  fields, the main module is module-relative — intake detects and records
  which (`module_stub_fields`).
- disc-side module name of the main binary: `9th`; updater: `updater`.

## 3. Decryption (out-of-process boundary)

`~PSP` modules cannot be processed in-repo (AGENTS.md #1/#2: no decryption
code, no keys). Intake resolves a **user-supplied** decrypt tool, in this
order:

1. `--decrypt-tool 'CMD {in} {out}'` (a shell template; positional
   `CMD <in> <out>` also accepted when no placeholders are used);
2. `GUNGNIR_DECRYPT_TOOL` environment variable (same grammar);
3. `pspdecrypt` — on `PATH` or `~/.local/bin/pspdecrypt` — invoked as
   `pspdecrypt -o <out> <in>` (GPL-3.0; build it yourself from
   [John-K/pspdecrypt](https://github.com/John-K/pspdecrypt);
   it is never committed to this repository);
4. a plaintext `BOOT.BIN` sibling next to `EBOOT.BIN`, used only when it
   actually parses as an ELF (verified, not trusted).

Decrypted forms are written under `out/modules/<path>.decrypted.elf` and then
validated by the same ELF gate used for plaintext modules
(`ELF_INVALID`, `PRX_SHAPE` checks; `ENCRYPTED_EXECUTABLE` if the decrypt
tool produced non-plaintext output; `DECRYPT_REQUIRED` when no tool was
found; `DECRYPT_TOOL_FAILED` if the external tool exits nonzero).

Nothing game-derived leaves `build/`; `tools/repository_check.py` must stay
green after every intake run (TC-3.4 asserts this in tests).

## 4. Tests

```sh
python3 -m unittest tests.test_intake -v      # TC-3.1..TC-3.4
```

Fixtures use synthetic blobs (our authorship); tests needing the private
disc run only when `game.iso` is present locally and fail-closed (exit 77)
in CI.

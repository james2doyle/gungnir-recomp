#!/usr/bin/env python3
"""repository_check.py — the IP gate (plan T1, azazel6-style).

Fail-closed audit of what is about to live in this repository. Checks the
working set (`git ls-files --cached --others --exclude-standard`, i.e. staged,
tracked, and untracked-but-not-ignored files):

  1. Forbidden extensions (disc images, ELF/PRX, game media, driver blobs).
  2. Magic-byte sniffing (ELF, ~PSP, CHD, PGF, PBP, PE, ISO 9660) so a
     renamed blob cannot slip past the extension check.
  3. Blob size cap (1 MiB) — game data and generated AOT code are large.
  4. Secret material (PEM private keys, common API-token shapes).
  5. Ignore-rule probes: representative game paths MUST be matched by
     .gitignore. This is the regression test for the .gitignore itself —
     if someone edits the rules and drops a pattern, the gate goes red.

A clean tree exits 0 with a summary; any violation exits 1 and names every
offending file. Run before every commit; CI runs it on every push.

Usage: python3 tools/repository_check.py [--repo PATH]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

# --- 1. Forbidden extensions ---------------------------------------------
FORBIDDEN_EXTENSIONS = {
    # Disc images & dumps
    ".iso": "game disc image", ".chd": "compressed disc image",
    ".cso": "compressed ISO", ".zso": "compressed ISO",
    ".pbp": "PSP package (contains game executable)",
    # Console executables/modules (encrypted or decrypted, both are derived)
    ".elf": "ELF executable", ".prx": "PSP module", ".bin": "binary blob",
    ".pmf": "PSP movie", ".sfo": "PSP metadata",
    # Game media/assets
    ".at3": "ATRAC3 audio", ".vag": "VAG audio", ".gim": "GIM texture",
    ".pgf": "PGF font (Sony font data)", ".dar": "game archive",
    ".tpl": "TPL texture container",
    # Binary blobs (build from pinned source instead; plan T2)
    ".zip": "archive/blob", ".jar": "Java archive",
    ".so": "shared library", ".dll": "shared library", ".dylib": "shared library",
    ".exe": "PE executable", ".apk": "Android package", ".aab": "Android bundle",
    # Keys & credentials
    ".jks": "keystore", ".pem": "key material",
}

# --- 2. Magic signatures --------------------------------------------------
# (offset, bytes, description) — checked against each file's content.
MAGIC_SIGNATURES = [
    (0, b"\x7fELF", "ELF executable"),
    (0, b"~PSP", "PSP encrypted module (EBOOT/PRX)"),
    (0, b"MComprHD", "MAME CHD disc image"),
    (0, b"PGF0", "PGF font (Sony font data)"),
    (0, b"\x00PBP", "PBP package"),
]
# The PE DOS stub is a SUBSTRING search, so the needle must be split in this
# source or the gate flags its own file (it did once — reproduced by
# tests: test_work_repo_passes_the_gate). Concatenated at runtime only.
PE_DOS_STUB = b"This program cannot be run in " + b"DOS mode"
ISO_MAGIC_OFFSET = 0x8001  # sector 16: 0x01 volume-descriptor byte + "CD001"
ISO_MAGIC_LEAD = 0x01
ISO_MAGIC = b"CD001"

# --- 3. Size cap ----------------------------------------------------------
MAX_BLOB_BYTES = 1024 * 1024  # 1 MiB

# --- 4. Secret patterns (built so this source file cannot self-match) -----
SECRET_PATTERNS = [
    (re.compile(rb"BEGIN (RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY"), "PEM private key"),
    (re.compile(rb"AKIA[0-9A-Z]{16}"), "AWS access key id"),
    (re.compile(rb"ghp_[A-Za-z0-9]{36}"), "GitHub token"),
    (re.compile(rb"github_pat_[A-Za-z0-9_]{22,}"), "GitHub token"),
]

# --- 5. Ignore-rule probes -----------------------------------------------
# Representative paths from every protected class. Each MUST be ignored by
# .gitignore. If a rule is ever weakened or deleted, this gate goes red.
IGNORE_PROBES = [
    "game.chd",
    "GAME.ISO",
    "build/game/x.iso",
    "PSP_GAME/SYSDIR/EBOOT.BIN",
    "module.elf",
    "module.prx",
    "disc.dar",
    "font.pgf",
    "movie.pmf",
    "keys/psp_keys.txt",
    "generated/gungnir_recomp.c",
    "profiles/gungnir/local/hashes.json",
    "drivers/turnip.zip",
    "android/local.properties",
    "memstick/PSP/SAVEDATA/ULUS10592/DATA.BIN",
    "savestates/slot1.state",
    "logs/boot.trace",
]


def git(repo, *args, text=True):
    return subprocess.run(["git", *args], cwd=str(repo), capture_output=True, text=text)


def collect_files(repo):
    """Staged + tracked + untracked-but-not-ignored files (relative paths)."""
    res = git(repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard", text=False)
    if res.returncode != 0:
        raise SystemExit(f"FAIL: git ls-files failed: {res.stderr.decode(errors='replace')}")
    return [p.decode() for p in res.stdout.split(b"\0") if p]


def check_ignore_probes(repo):
    violations = []
    for probe in IGNORE_PROBES:
        res = git(repo, "check-ignore", "-q", "--", probe)
        if res.returncode != 0:
            violations.append(
                f"FAIL: ignore-rule probe '{probe}' is not ignored by .gitignore — "
                f"the rule protecting it is missing or broken (ignore-rule regression)"
            )
    return violations


def check_file(repo, rel):
    """Extension + magic + size + secret checks for one file. Returns violations."""
    violations = []
    path = repo / rel
    suffix = path.suffix.lower()

    ext_reason = FORBIDDEN_EXTENSIONS.get(suffix)
    if ext_reason:
        violations.append(f"FAIL: {rel} — forbidden extension '{suffix}' ({ext_reason})")
        return violations  # content checks add nothing over a named extension hit

    try:
        data = path.read_bytes()
    except OSError as exc:
        return [f"FAIL: {rel} — cannot read staged file: {exc}"]

    for offset, magic, desc in MAGIC_SIGNATURES:
        if data[offset:offset + len(magic)] == magic:
            violations.append(f"FAIL: {rel} — content is a {desc} (magic bytes)")
            return violations
    if PE_DOS_STUB in data[:65536]:
        violations.append(f"FAIL: {rel} — content is a PE executable (DOS stub)")
        return violations
    if (len(data) > ISO_MAGIC_OFFSET + len(ISO_MAGIC)
            and data[ISO_MAGIC_OFFSET - 1] == ISO_MAGIC_LEAD
            and data[ISO_MAGIC_OFFSET:ISO_MAGIC_OFFSET + len(ISO_MAGIC)] == ISO_MAGIC):
        violations.append(f"FAIL: {rel} — content is an ISO 9660 disc image (sector-16 magic)")
        return violations

    if len(data) > MAX_BLOB_BYTES:
        violations.append(
            f"FAIL: {rel} — blob is {len(data)} bytes, over the {MAX_BLOB_BYTES} cap "
            f"(game data and generated code must never be committed)"
        )
        return violations

    for pattern, desc in SECRET_PATTERNS:
        if pattern.search(data):
            violations.append(f"FAIL: {rel} — contains secret material ({desc})")
            return violations

    return violations


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=".", help="repository root (default: cwd)")
    args = ap.parse_args()
    repo = Path(args.repo).resolve()

    if git(repo, "rev-parse", "--git-dir").returncode != 0:
        print(f"FAIL: {repo} is not a git repository", file=sys.stderr)
        return 1

    violations = check_ignore_probes(repo)

    files = collect_files(repo)
    for rel in sorted(files):
        violations.extend(check_file(repo, rel))

    if violations:
        for v in violations:
            print(v, file=sys.stderr)
        print(f"\nrepository_check: FAIL — {len(violations)} violation(s) across {len(files)} file(s)",
              file=sys.stderr)
        return 1

    print(f"repository_check: PASS — {len(files)} file(s) clean, "
          f"{len(IGNORE_PROBES)} ignore-rule probes intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Intake orchestrator — T3 pipeline: game.iso -> verified local build tree.

Covers plan TC-3.1..TC-3.4. The C++ reader tool (intake_tool) does the heavy
lifting; this script owns the identity gate, the decrypt hook boundary, and
manifest emission. All artifacts land under the requested output directory
(build/ is gitignored); nothing game-derived is ever written outside it.

Disc policy (plan §1): ISO input only. .chd is converted user-side
(CHDStudio or `chdman extractcd`); intake refuses CHD with a named error.
Decryption runs out-of-process with user-supplied tooling — no decryption
code or key material ships in this repository (AGENTS.md #1/#2).

Subcommands:
  intake.py verify --iso GAME.iso [--expect-size N] [--expect-crc HEX]
  intake.py run    --iso GAME.iso --out DIR [--expect-size N] [--expect-crc HEX]
                    [--decrypt-tool TOOL]

Errors are named and printed to stderr as `INTAKE_ERR <NAME>: detail`;
exit code is nonzero (2) for all failure modes, never silent.
"""
import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
TOOL = REPO / "build" / "tools" / "intake" / "intake_tool"

# Gungnir (USA, ULUS-10592) disc identity — redump-recorded facts (plan §2).
DEFAULT_EXPECT_SIZE = 262766592
DEFAULT_EXPECT_CRC = "183F3EDC"
EXPECTED_DISC_ID = "ULUS-10592"

CHD_GUIDANCE = (
    "convert the CHD once, user-side: CHDStudio (GUI, recommended) or "
    "`chdman extractcd`; see docs/INTAKE.md"
)

MODULE_EXTRA = 8  # ~PSP magic bytes we sniff before trusting an ELF reader


class IntakeError(Exception):
    def __init__(self, name, detail):
        super().__init__(f"{name}: {detail}")
        self.name = name
        self.detail = detail


def die(name, detail):
    raise IntakeError(name, detail)


def tool_path():
    p = TOOL
    if not p.exists():
        die("TOOL_NOT_BUILT", f"intake reader tool missing at {p}; build with: cmake -S . -B build && cmake --build build --target intake_tool")
    return str(p)


def run_tool(args, expected_ok=True):
    proc = subprocess.run([tool_path(), *args], capture_output=True, text=True)
    if expected_ok and proc.returncode != 0:
        err = proc.stderr.strip()
        if err.startswith("ERR "):
            name, _, detail = err[4:].partition(" ")
            die(name, detail)
        die("TOOL_FAILED", err or proc.stdout.strip())
    return proc


# --- identity gate -----------------------------------------------------------


def crc32_file(path):
    crc = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def verify(iso, expect_size=DEFAULT_EXPECT_SIZE, expect_crc=DEFAULT_EXPECT_CRC):
    iso = Path(iso)
    if not iso.is_file():
        die("ISO_NOT_FOUND", str(iso))
    if iso.suffix.lower() == ".chd":
        die("CHD_NOT_SUPPORTED", f"{iso.name} — {CHD_GUIDANCE}")
    actual_size = iso.stat().st_size
    expect_size = int(expect_size)
    if actual_size != expect_size:
        die("IDENTITY_MISMATCH",
            f"size {actual_size} != expected {expect_size} (transforming tools are verified, not trusted)")
    actual_crc = f"{crc32_file(iso):08X}"
    expect_crc = str(expect_crc).upper().removeprefix("0X")
    if actual_crc != expect_crc:
        die("IDENTITY_MISMATCH",
            f"CRC32 {actual_crc} != expected {expect_crc} (transforming tools are verified, not trusted)")
    return {"identity": "verified", "size": actual_size, "crc32": actual_crc}


# --- extraction + manifest ---------------------------------------------------


def extract(iso, out_dir):
    out = out_dir / "files"
    proc = run_tool(["iso-extract", "--iso", str(iso), "--out", str(out)])
    files = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[0] == "FILE":
            files.append({"path": parts[1], "size": int(parts[2])})
    return files


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def sfo_info(path):
    if not path.exists():
        die("SFO_MISSING", "PSP_GAME/PARAM.SFO absent from disc extract")
    proc = run_tool(["sfo-info", str(path)])
    sfo = {}
    for line in proc.stdout.splitlines():
        key, _, value = line.partition("=")
        if key and value is not None:
            sfo[key] = value
    return sfo


def load_module_record(elf_file, source_tag, out_dir, name):
    """elf-inspect an existing plaintext module file (decrypted output or
    extracted plaintext). No re-copying: same-named modules at different
    disc paths can't trample each other's decrypted forms.
    """
    candidate = Path(elf_file)
    proc = subprocess.run([tool_path(), "elf-inspect", str(candidate)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        stderr = proc.stderr.strip()
        if "ENCRYPTED" in stderr:
            die("ENCRYPTED_EXECUTABLE", f"{name}: still ~PSP after decrypt hook")
        die("ELF_INVALID", f"{name}: {stderr}")
    record = {"name": name, "path": str(candidate.relative_to(out_dir))}
    if source_tag:
        record["source"] = source_tag
    imports = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "entry":
            record["entry"] = parts[1]
        elif parts[0] == "module_name":
            record["module_name"] = parts[1]
        elif parts[0] == "module_gp":
            record["module_gp"] = parts[1]
        elif parts[0] == "segment" and len(parts) >= 7:
            record.setdefault("segments", []).append({
                "index": int(parts[1]), "offset": parts[2], "vaddr": parts[3],
                "file_size": int(parts[4]), "memory_size": int(parts[5]),
                "flags": parts[6],
            })
        elif parts[0] == "reloc_section":
            # Section name may be empty (stripped ELFs): "reloc_section <type> <n>".
            if len(parts) == 4:
                sec_name, sec_type, sec_entries = parts[1], parts[2], int(parts[3])
            else:
                sec_name, sec_type, sec_entries = "", parts[1], int(parts[2])
            record.setdefault("reloc_sections", []).append({
                "name": sec_name, "type": sec_type, "entries": sec_entries,
            })
        elif parts[0] == "reloc_R_MIPS_32":
            record["reloc_r_mips_32"] = int(parts[1])
        elif parts[0] == "reloc_R_MIPS_HI16":
            record["reloc_r_mips_hi16"] = int(parts[1])
        elif parts[0] == "reloc_R_MIPS_LO16":
            record["reloc_r_mips_lo16"] = int(parts[1])
        elif parts[0] == "reloc_other":
            record["reloc_other"] = int(parts[1])
        elif parts[0] == "reloc_sections_entries":
            record["reloc_entries"] = int(parts[1])
        elif parts[0] == "imports":
            record["imports_count"] = int(parts[1])
        elif parts[0] == "import":
            imports.append({"library": parts[1], "nid": parts[2], "stub": parts[3]})
    if imports:
        record["imports"] = imports
    return record


def decrypt_hook(encrypted_path, out_dir, name, decrypt_tool, token):
    """Out-of-process decrypt boundary (plan §0.3): resolution order —

    1. explicit --decrypt-tool CLI template ('{in}'/'{out}' placeholders,
       else positional <in> <out>);
    2. GUNGNIR_DECRYPT_TOOL environment variable (same template grammar);
    3. `pspdecrypt` on PATH or ~/.local/bin — invoked as
       `pspdecrypt -o <out> <in>`.

    The tool is the user's, built outside this repository; nothing here
    decrypts. A missing/corrupt tool is a loud named failure.
    """
    encrypted = Path(encrypted_path)
    if name.lower() == "eboot.bin":
        candidate = encrypted.parent / "BOOT.BIN"
        if candidate.exists() and _looks_like_elf(candidate):
            return candidate, "boot.bin-plaintext-sibling"
    if not decrypt_tool:
        decrypt_tool = os.environ.get("GUNGNIR_DECRYPT_TOOL")
    if not decrypt_tool:
        found = shutil.which("pspdecrypt") or shutil.which(
            str(Path.home() / ".local" / "bin" / "pspdecrypt"))
        if found:
            decrypt_tool = f"{found} -o {{out}} {{in}}"
        else:
            die("DECRYPT_REQUIRED",
                f"{name} is ~PSP encrypted and no decrypt tool was found. "
                "Supply one out-of-process: pspdecrypt on PATH (or "
                "~/.local/bin/pspdecrypt), --decrypt-tool 'CMD {in} {out}', "
                "GUNGNIR_DECRYPT_TOOL, or PPSSPP 'Dump decrypted "
                "EBOOT.BIN'; see docs/INTAKE.md (no decryption ships in-repo)")
    out_file = out_dir / "modules" / f"{token}.decrypted.elf"
    out_dir_modules = out_file.parent
    out_dir_modules.mkdir(parents=True, exist_ok=True)
    argv = _spawn_decrypt_argv(decrypt_tool, encrypted, out_file)
    proc = subprocess.run(argv, capture_output=True, text=True)
    if proc.returncode != 0:
        die("DECRYPT_TOOL_FAILED",
            f"{name}: decrypt tool exited {proc.returncode}: "
            f"{(proc.stderr or proc.stdout).strip()}")
    if not _looks_like_elf(out_file):
        die("ENCRYPTED_EXECUTABLE", f"decrypt tool output for {name} is not a plaintext ELF")
    return out_file, f"decrypt-tool:{argv[0]}"


def _spawn_decrypt_argv(decrypt_tool, input_path, output_path):
    tokens = shlex.split(decrypt_tool)
    if not tokens:
        die("DECRYPT_REQUIRED", "empty --decrypt-tool")
    if any("{in}" in t or "{out}" in t or "{}" in t for t in tokens):
        argv = []
        for t in tokens:
            argv.append(t.replace("{in}", str(input_path))
                        .replace("{out}", str(output_path))
                        .replace("{}", str(input_path)))
        return argv
    return [*tokens, str(input_path), str(output_path)]


_MAGIC_ELF = b"\x7fELF"
_MAGIC_PSP = b"~PSP"


def _looks_like_elf(path):
    with open(path, "rb") as f:
        blob = f.read(MODULE_EXTRA)
    return blob.startswith(_MAGIC_ELF)


# --- run pipeline -------------------------------------------------------------


def run_pipeline(iso, out_dir, decrypt_tool=None, expect_size=DEFAULT_EXPECT_SIZE,
                 expect_crc=DEFAULT_EXPECT_CRC):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    identity = verify(iso, expect_size, expect_crc)
    files = extract(iso, out_dir)

    by_path = {f["path"]: f for f in files}
    for f in files:
        local = out_dir / "files" / f["path"]
        if local.stat().st_size != f["size"]:
            die("EXTRACT_SIZE_MISMATCH", f"{f['path']}: {local.stat().st_size} != manifest {f['size']}")
        f["sha256"] = sha256_file(local)

    sfo = sfo_info(out_dir / "files" / "PSP_GAME" / "PARAM.SFO")
    disc_id = sfo.get("DISC_ID", "").strip()
    norm = lambda s: s.upper().replace("-", "").replace("_", "")
    if norm(disc_id) != norm(EXPECTED_DISC_ID):
        die("DISC_ID_MISMATCH",
            f"PARAM.SFO DISC_ID {disc_id!r} does not match {EXPECTED_DISC_ID!r}")

    # SYSDIR executables inventory (EBOOT.BIN / BOOT.BIN / *.prx under SYSDIR).
    module_paths = [f["path"] for f in files
                    if f["path"].startswith("PSP_GAME/SYSDIR/")
                    and (f["path"].rsplit("/", 1)[-1].lower() in ("eboot.bin", "boot.bin")
                         or f["path"].lower().endswith(".prx"))]
    modules = []
    for path in sorted(module_paths):
        local = out_dir / "files" / path
        name = path.rsplit("/", 1)[-1]
        token = re.sub(r"[^A-Za-z0-9._-]+", "__", path.removeprefix("PSP_GAME/"))
        if _is_encrypted(local):
            blob, how = decrypt_hook(local, out_dir, name, decrypt_tool, token)
            rec = load_module_record(blob, f"decrypted", out_dir, name)
            rec["disc_path"] = path
            rec["encrypted_source"] = True
            rec["decrypt_source"] = how
            modules.append(rec)
        elif _looks_like_elf(local):
            rec = load_module_record(local, "plaintext", out_dir, name)
            rec["disc_path"] = path
            modules.append(rec)
        else:
            # Neither ~PSP nor ELF (this disc's BOOT.BIN is such a case):
            # recorded explicitly as an unverified artifact — a counted,
            # named gap, never a silent pass or a fake "validated" row.
            modules.append({
                "name": name,
                "disc_path": path,
                "status": "UNRECOGNIZED_SHAPE",
                "note": ("neither ~PSP nor ELF; not validated by intake "
                         "(counted gap; PPSSPP/priiloader-style dumps may "
                         "explain it in a later bring-up pass)"),
            })

    manifest = {
        "disc": identity,
        "sfo": {"disc_id": disc_id, **sfo},
        "files": sorted(files, key=lambda f: f["path"]),
        "modules": modules,
    }
    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"manifest: {manifest_path} "
          f"({len(files)} files, {len(modules)} modules)")
    return manifest


def _is_encrypted(path):
    with open(path, "rb") as f:
        blob = f.read(MODULE_EXTRA)
    return blob.startswith(_MAGIC_PSP)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Gungnir intake orchestrator (T3)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_verify = sub.add_parser("verify", help="identity gate (size + CRC32)")
    p_verify.add_argument("--iso", required=True)
    p_verify.add_argument("--expect-size", default=DEFAULT_EXPECT_SIZE)
    p_verify.add_argument("--expect-crc", default=DEFAULT_EXPECT_CRC)

    p_run = sub.add_parser("run", help="full pipeline: verify + extract + manifest")
    p_run.add_argument("--iso", required=True)
    p_run.add_argument("--out", required=True)
    p_run.add_argument("--expect-size", default=DEFAULT_EXPECT_SIZE)
    p_run.add_argument("--expect-crc", default=DEFAULT_EXPECT_CRC)
    p_run.add_argument("--decrypt-tool", default=os.environ.get("GUNGNIR_DECRYPT_TOOL"),
                       help="user-supplied decrypt command; supports '{in}'/'{out}' placeholders")

    p_sfo = sub.add_parser("sfo-info", help="PARAM.SFO raw print (reader passthrough)")
    p_sfo.add_argument("sfo")

    p_inspect = sub.add_parser("elf-inspect", help="elf-inspect a module (passthrough)")
    p_inspect.add_argument("elf")

    args = parser.parse_args(argv)
    try:
        if args.cmd == "verify":
            identity = verify(args.iso, args.expect_size, args.expect_crc)
            print(f"IDENTITY_OK size={identity['size']} crc32={identity['crc32']}")
        elif args.cmd == "run":
            run_pipeline(args.iso, args.out, args.decrypt_tool,
                         args.expect_size, args.expect_crc)
        elif args.cmd == "sfo-info":
            proc = run_tool(["sfo-info", args.sfo])
            sys.stdout.write(proc.stdout)
        elif args.cmd == "elf-inspect":
            proc = run_tool(["elf-inspect", args.elf])
            sys.stdout.write(proc.stdout)
    except IntakeError as e:
        print(f"INTAKE_ERR {e.name}: {e.detail}", file=sys.stderr)
        return 2
    except Exception as e:  # unexpected — still named, never silent
        print(f"INTAKE_ERR UNEXPECTED: {e}", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())

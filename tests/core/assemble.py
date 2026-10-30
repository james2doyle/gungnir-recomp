#!/usr/bin/env python3
"""Assemble a synthetic Allegrex fixture (T4) into a minimal PSP PRX ELF.

Input format (`tests/core/fixtures/*.mips`). Fixtures are linked as plain
executables at the standard PSP user base 0x08804000 (non-PRX, one absolute
PT_LOAD), so guest literals are valid runtime addresses and both tiers use the
same addresses:

    # name: arith_basic
    # sentinel: 0x3F00          # dispatch target the fixture returns to
    # sp: 0x8000                # initial $sp (harness sets it)
    # mem: 0x1000 = DEADBEEF    # initial guest memory (hex bytes)
    # digest: 0x1000:0x1010     # memory windows included in state digests
    # trace: 0x0                # dispatch PCs recorded in the trace
    0x24081000                  # addiu t0, zero, 0x1000
    ...

Outputs next to <prefix>:
  <prefix>.elf   minimal ET_EXEC ELF (one PT_LOAD at 0x08804000)
  <prefix>.csv   functions.csv for yakumo_codegen  (name,address,size)
  <prefix>.json  metadata consumed by tests/test_core_fixtures.py
"""
import json
import struct
import sys
from pathlib import Path

PT_LOAD = 1
PF_RWX = 7
CODE_OFFSET = 0x100
LOAD_VADDR = 0x08804000


def parse_fixture(path: Path):
    meta = {"name": path.stem, "sentinel": None, "sp": None,
            "mem": [], "digest": [], "trace": []}
    words = []
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        if raw.lstrip().startswith("#"):
            body = raw.lstrip("#").strip()
            if not body or ":" not in body:
                continue
            key, _, value = body.partition(":")
            key, value = key.strip(), value.strip()
            if key == "name":
                meta["name"] = value
            elif key in ("sentinel", "sp"):
                meta[key] = int(value, 16)
            elif key == "mem":
                addr, _, hex_bytes = value.partition("=")
                meta["mem"].append({"addr": int(addr.strip(), 16),
                                    "hex": hex_bytes.strip()})
            elif key == "digest":
                start, _, end = value.partition(":")
                meta["digest"].append({"start": int(start, 16),
                                       "end": int(end, 16)})
            elif key == "trace":
                meta["trace"].append(int(value, 16))
            # anything else is prose in a comment, not a header
            continue
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        for token in line.split():
            if not token.startswith("0x"):
                raise SystemExit(f"{path}:{lineno}: word {token!r} is not hex")
            word = int(token, 16)
            if not 0 <= word <= 0xFFFFFFFF:
                raise SystemExit(f"{path}:{lineno}: word out of range")
            words.append(word)

    if not words:
        raise SystemExit(f"{path}: no code words")
    if meta["sentinel"] is None or meta["sp"] is None:
        raise SystemExit(f"{path}: sentinel and sp headers are required")
    code_size = len(words) * 4
    if code_size > 0x3000:
        raise SystemExit(f"{path}: code must stay under 0x3000 bytes")
    code_end = LOAD_VADDR + code_size
    if code_end > 0x0A000000:
        raise SystemExit(f"{path}: code runs past user RAM")
    for key in ("sentinel", "sp"):
        value = meta[key]
        if value < code_end or value >= 0x0A000000:
            raise SystemExit(f"{path}: {key} {value:#x} must be outside the code"
                             f" and inside user RAM")
    # The initial dispatch exists only in the AOT tier; the harness emits a
    # synthetic start record, so the trace set starts empty.
    if not meta["trace"]:
        meta["trace"] = []
    meta["entry"] = LOAD_VADDR
    meta["size"] = code_size
    return meta, words


def build_elf(words):
    code = b"".join(struct.pack("<I", w) for w in words)
    ident = b"\x7fELF" + bytes([1, 1, 1]) + b"\x00" * 9
    elf_header = struct.pack(
        "<16sHHIIIIIHHHHHH", ident,
        2,                 # e_type: ET_EXEC (absolute, no relocation)
        8,                 # e_machine: MIPS
        1,                 # e_version
        LOAD_VADDR,        # e_entry
        52,                # e_phoff
        0,                 # e_shoff
        0,                 # e_flags
        52,                # e_ehsize
        32,                # e_phentsize
        1,                 # e_phnum
        40, 0, 0)          # e_shentsize, e_shnum, e_shstrndx
    phdr = struct.pack("<IIIIIIII", PT_LOAD, CODE_OFFSET, LOAD_VADDR, LOAD_VADDR,
                       len(code), len(code), PF_RWX, 4)
    blob = elf_header + phdr
    blob += b"\x00" * (CODE_OFFSET - len(blob))
    return blob + code


def main(argv):
    if len(argv) != 3:
        raise SystemExit("usage: assemble.py <fixture.mips> <output-prefix>")
    src = Path(argv[1])
    prefix = Path(argv[2])
    meta, words = parse_fixture(src)
    prefix.parent.mkdir(parents=True, exist_ok=True)

    (prefix.with_suffix(".elf")).write_bytes(build_elf(words))
    (prefix.with_suffix(".csv")).write_text(
        f"{meta['name']},0x{LOAD_VADDR:X},0x{meta['size']:X}\n")
    (prefix.with_suffix(".json")).write_text(json.dumps(meta, indent=2) + "\n")
    print(f"assembled {meta['name']}: {len(words)} words -> "
          f"{prefix.with_suffix('.elf')}")


if __name__ == "__main__":
    main(sys.argv)

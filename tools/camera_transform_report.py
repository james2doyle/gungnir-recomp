#!/usr/bin/env python3
"""Summarise a GUNGNIR_TRACE_CAMERA log's per-draw transform stream.

Groups GE world (0x3A/0x3B), view (0x3C/0x3D) and bone (0x2A/0x2B) commands
into 12-lane matrix blocks, decodes f24 (top 3 bytes of an IEEE float) and
reports how many DISTINCT matrices exist per kind and where they are uploaded.

The decisive question for the matrix-interpolation direction: are these
matrices actually per-frame/per-object motion, or constant? A title that
projects vertices on the CPU has nothing to interpolate at the matrix level.

Usage: python3 tools/camera_transform_report.py <run.log>
Private input: the log comes from a local bounded boot; never committed.
"""
import argparse
import collections
import re
import struct
import sys
from pathlib import Path

LINE = re.compile(
    r"camera_(?:projection|transform)\]\s*list=(0x[0-9A-Fa-f]+) "
    r"command_pc=(0x[0-9A-Fa-f]+) opcode=(0x[0-9A-Fa-f]+) word=(0x[0-9A-Fa-f]+)")

INDEX = {0x2A: "bone", 0x3A: "world", 0x3C: "view", 0x3E: "projection"}
UPLOAD = {0x2B: "bone", 0x3B: "world", 0x3D: "view", 0x3F: "projection"}


def f24(word):
    return struct.unpack("<f", struct.pack("<I", (word & 0xFFFFFF) << 8))[0]


def analyse(path):
    blocks = []
    current = None
    for line in path.read_text().splitlines():
        match = LINE.search(line)
        if match is None:
            continue
        pc = int(match.group(2), 16)
        opcode = int(match.group(3), 16)
        word = int(match.group(4), 16)
        if opcode in INDEX:
            if current is not None and current[2]:
                blocks.append(current)
            current = [INDEX[opcode], pc, []]
        elif opcode in UPLOAD and current is not None and UPLOAD[opcode] == current[0]:
            current[2].append(f24(word))
    if current is not None and current[2]:
        blocks.append(current)

    by_kind = collections.defaultdict(list)
    for kind, pc, values in blocks:
        by_kind[kind].append((pc, tuple(round(v, 4) for v in values)))
    print(f"log={path.name} transform_blocks={len(blocks)}")
    for kind in ("world", "view", "bone"):
        entries = by_kind.get(kind, [])
        distinct = collections.Counter(matrix for _, matrix in entries)
        sites = collections.Counter(pc for pc, _ in entries)
        full = sum(1 for _, matrix in entries if len(matrix) == 12)
        print(f"\n{kind}: blocks={len(entries)} distinct_matrices={len(distinct)} "
              f"distinct_sites={len(sites)} full_12_lane_blocks={full}")
        for pc, count in sites.most_common(6):
            print(f"  site={hex(pc)} count={count}")
        for matrix, count in distinct.most_common(4):
            print(f"  count={count} matrix={matrix}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    if not args.log.is_file():
        print(f"camera_transform_report: missing log {args.log}", file=sys.stderr)
        return 2
    analyse(args.log)
    return 0


if __name__ == "__main__":
    sys.exit(main())

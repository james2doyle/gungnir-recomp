#!/usr/bin/env python3
"""c64gg6n7 — analyse a camera-trace log produced by GUNGNIR_TRACE_CAMERA=1.

Reads the [camera_projection] lines from a bounded host boot, groups them into
16-lane projection matrices (0x3E index reset + 16 0x3F uploads), decodes the
f24 arguments (GE f24 == top 3 bytes of an IEEE float) and reports the distinct
matrices and their upload sites with instance counts.

Usage: python3 tools/camera_trace_report.py <run.log> [--sites]
Private input: the log comes from a local bounded run; it is never committed.
"""
import argparse
import collections
import re
import struct
import sys
from pathlib import Path

LINE = re.compile(
    r"camera_projection\] list=(0x[0-9A-Fa-f]+) command_pc=(0x[0-9A-Fa-f]+) "
    r"opcode=(0x[0-9A-Fa-f]+) word=(0x[0-9A-Fa-f]+)")


def f24(word):
    return struct.unpack("<f", struct.pack("<I", (word & 0xFFFFFF) << 8))[0]


def analyse(path, show_sites):
    matrices = []
    starts = collections.Counter()
    current = None
    for line in path.read_text().splitlines():
        match = LINE.search(line)
        if match is None:
            continue
        lst, pc, opcode, word = match.groups()
        pc = int(pc, 16)
        opcode = int(opcode, 16)
        word = int(word, 16)
        if opcode == 0x3E:
            if current is not None and len(current[2]) == 16:
                matrices.append(current)
            starts[(lst, pc)] += 1
            current = [lst, pc, []]
        elif opcode == 0x3F and current is not None:
            current[2].append(word & 0xFFFFFF)
    if current is not None and len(current[2]) == 16:
        matrices.append(current)

    print(f"log={path.name} complete_matrices={len(matrices)}")
    distinct = collections.Counter(tuple(m[2]) for m in matrices)
    print(f"distinct_matrices={len(distinct)}")
    for lanes, count in distinct.most_common():
        values = [f24(w) for w in lanes]
        print(f"\n  count={count} lanes(f24 args)={[hex(w) for w in lanes]}")
        for row in range(4):
            print("    " + "  ".join(f"{v: .6f}" for v in values[row * 4:row * 4 + 4]))
    if show_sites:
        print(f"\n0x3E block starts ({len(starts)} distinct):")
        for (lst, pc), count in sorted(starts.items()):
            print(f"  list={lst} pc={hex(pc)} count={count}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("log", type=Path)
    parser.add_argument("--sites", action="store_true")
    args = parser.parse_args()
    if not args.log.is_file():
        print(f"camera_trace_report: missing log {args.log}", file=sys.stderr)
        return 2
    analyse(args.log, args.sites)
    return 0


if __name__ == "__main__":
    sys.exit(main())

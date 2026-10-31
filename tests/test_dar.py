#!/usr/bin/env python3
"""T12b — DAR list/extract and size-preserving in-place repack contracts.

Synthetic archives are our authorship. The user's INI.DAR is private input
under gitignored build/; when absent the real-archive case skips with
dar_fixture_missing.
"""
import hashlib
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "dar.py"
REAL_DAR = REPO / "build" / "game" / "files" / "PSP_GAME" / "USRDIR" / "INI.DAR"


def build_archive(entries):
    """entries: [(name, payload, kind)] with kind stored-zero/stored/zlib."""
    table_offset = 16
    names_offset = table_offset + 16 * len(entries)
    names = b"".join(name.encode("ascii") + b"\0" for name, _, _ in entries)
    data_start = (names_offset + len(names) + 3) & ~3
    table = bytearray()
    blobs = bytearray()
    name_offset = names_offset
    for name, payload, kind in entries:
        if kind == "stored-zero":
            stored, decompressed, blob = 0, len(payload), payload
        elif kind == "stored":
            stored, decompressed, blob = len(payload), len(payload), payload
        elif kind == "zlib":
            blob = zlib.compress(payload, 9)
            stored, decompressed = len(blob), len(payload)
        else:
            raise ValueError(kind)
        data_offset = data_start + len(blobs)
        table += struct.pack("<IIII", name_offset, stored, decompressed, data_offset)
        name_offset += len(name.encode("ascii")) + 1
        blobs += blob
    header = struct.pack("<IIII", len(entries), data_start, names_offset, table_offset)
    padding = b"\0" * (data_start - names_offset - len(names))
    return header + bytes(table) + names + padding + bytes(blobs)


class DarToolTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="gungnir-dar-")
        self.root = Path(self.temp.name)
        self.archive = self.root / "TEST.DAR"
        self.entries = [
            ("stored-zero.bin", b"zero-slot-payload", "stored-zero"),
            ("stored.bin", b"stored-slot-payload", "stored"),
            ("packed.bin", b"packed-payload-" * 8, "zlib"),
        ]
        self.archive.write_bytes(build_archive(self.entries))

    def tearDown(self):
        self.temp.cleanup()

    def run_tool(self, *args):
        return subprocess.run([sys.executable, str(TOOL), *args],
                              capture_output=True, text=True, timeout=120)

    def extract(self, out_name="out"):
        out = self.root / out_name
        proc = self.run_tool("extract", str(self.archive), "--out", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return out

    def test_list_and_info(self):
        proc = self.run_tool("list", str(self.archive))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        for name, _, _ in self.entries:
            self.assertIn(name, proc.stdout)
        info = self.run_tool("info", str(self.archive))
        self.assertEqual(info.returncode, 0, info.stderr)
        self.assertIn("entries=3", info.stdout)

    def test_extract_then_repack_is_byte_identical(self):
        out = self.extract()
        for name, payload, _ in self.entries:
            self.assertEqual((out / name).read_bytes(), payload, name)
        before = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        proc = self.run_tool("repack", str(self.archive), "--from", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        after = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        self.assertEqual(before, after, "an untouched repack must be byte-identical")
        self.assertIn("changed 0", proc.stderr)

    def test_rewrites_compressed_and_zero_slots(self):
        out = self.extract()
        size_before = self.archive.stat().st_size
        (out / "packed.bin").write_bytes(b"A" * 120)
        (out / "stored-zero.bin").write_bytes(b"zero-slot-CHANGED")
        self.assertEqual(len(b"zero-slot-CHANGED"), len(b"zero-slot-payload"))
        proc = self.run_tool("repack", str(self.archive), "--from", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(self.archive.stat().st_size, size_before, "repack must not resize the archive")
        second = self.extract("out2")
        self.assertEqual((second / "packed.bin").read_bytes(), b"A" * 120)
        self.assertEqual((second / "stored-zero.bin").read_bytes(), b"zero-slot-CHANGED")

    def test_short_compressed_replacement_is_rejected(self):
        out = self.extract()
        (out / "packed.bin").write_bytes(b"short-packed")
        proc = self.run_tool("repack", str(self.archive), "--from", str(out))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("DAR_COMPRESSED_SLOT_TOO_SMALL", proc.stderr)

    def test_stored_zero_length_change_is_rejected(self):
        out = self.extract()
        (out / "stored-zero.bin").write_bytes(b"longer-than-the-slot-payload")
        proc = self.run_tool("repack", str(self.archive), "--from", str(out))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("DAR_STORED_SIZE_CHANGED", proc.stderr)

    def test_oversized_compressed_replacement_is_rejected(self):
        out = self.extract()
        (out / "packed.bin").write_bytes(random.Random(0).randbytes(4096))
        proc = self.run_tool("repack", str(self.archive), "--from", str(out))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("DAR_ENTRY_TOO_LARGE", proc.stderr)

    def test_traversal_names_are_rejected(self):
        self.archive.write_bytes(build_archive([("../evil.bin", b"x", "stored-zero")]))
        proc = self.run_tool("list", str(self.archive))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("DAR_INVALID_NAME", proc.stderr)

    def test_real_ini_dar_round_trips(self):
        if not REAL_DAR.is_file():
            self.skipTest("dar_fixture_missing: build/game/files/PSP_GAME/USRDIR/INI.DAR")
        archive = self.root / "INI.DAR"
        archive.write_bytes(REAL_DAR.read_bytes())
        out = self.root / "real"
        proc = self.run_tool("extract", str(archive), "--out", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        before = hashlib.sha256(archive.read_bytes()).hexdigest()
        proc = self.run_tool("repack", str(archive), "--from", str(out))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        after = hashlib.sha256(archive.read_bytes()).hexdigest()
        self.assertEqual(before, after)


if __name__ == "__main__":
    unittest.main()

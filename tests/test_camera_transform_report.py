#!/usr/bin/env python3
"""Contracts for tools/camera_transform_report.py.

The decisive question for matrix interpolation is whether the guest's GE
transform matrices actually carry per-frame/per-object motion. This reader must
group the 0x3A/0x3B (world), 0x3C/0x3D (view), 0x2A/0x2B (bone) commands into
12-lane blocks, decode f24, and count DISTINCT matrices - a title that
projects vertices on the CPU has only a handful of constant matrices.

Red-first: written before the tool existed.
"""
import io
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import camera_transform_report as report  # noqa: E402


def line(pc, opcode, word):
    return (f"[camera_transform] list=0x09C51010 command_pc={hex(pc)} "
            f"opcode={hex(opcode)} word={hex(word)}\n")


def block(start, kind_index, kind_upload, values):
    out = line(start, kind_index, 0)  # index reset
    for i, v in enumerate(values):
        out += line(start + 4 + 4 * i, kind_upload, v)
    return out


class TestF24(unittest.TestCase):
    def test_f24_is_top_three_bytes_of_a_float(self):
        self.assertAlmostEqual(report.f24(0x3F8000), 1.0, places=6)
        self.assertAlmostEqual(report.f24(0xBF8000), -1.0, places=6)


class TestBlockGrouping(unittest.TestCase):
    def _run(self, text):
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
            path = Path(f.name)
        out = io.StringIO()
        with redirect_stdout(out):
            report.analyse(path)
        path.unlink()
        return out.getvalue()

    def test_a_12_lane_world_block_is_counted(self):
        values = [0x3F8000, 0, 0, 0, 0x3F8000, 0, 0, 0, 0x3F8000, 0, 0, 0]
        text = block(0x9C522B0, 0x3A, 0x3B, values)
        out = self._run(text)
        self.assertIn("transform_blocks=1", out)
        self.assertIn("world: blocks=1", out)
        self.assertIn("full_12_lane_blocks=1", out)

    def test_constant_matrices_are_reported_as_few_distinct(self):
        values = [0x3F8000, 0, 0, 0, 0x3F8000, 0, 0, 0, 0x3F8000, 0, 0, 0]
        text = block(0x9C522B0, 0x3A, 0x3B, values) * 5
        out = self._run(text)
        self.assertIn("world: blocks=5 distinct_matrices=1", out,
                      "repeating one constant matrix must not look like motion")

    def test_view_and_world_are_separate_kinds(self):
        v = [0x3F8000] + [0] * 11
        text = (block(0x9C5227C, 0x3C, 0x3D, v) +
                block(0x9C522B0, 0x3A, 0x3B, v))
        out = self._run(text)
        self.assertIn("world: blocks=1", out)
        self.assertIn("view: blocks=1", out)

    def test_log_without_a_space_after_the_tag_still_parses(self):
        # Earlier builds emitted "[camera_transform]list=" - the reader must
        # tolerate both forms so old logs remain analysable.
        values = [0x3F8000] + [0] * 11
        text = (f"[camera_transform]list=0x09C51010 command_pc=0x9C5227C "
                f"opcode=0x3C word=0x3C000000\n")
        for i, v in enumerate(values):
            text += (f"[camera_transform]list=0x09C51010 "
                     f"command_pc={hex(0x9C52280 + 4*i)} opcode=0x3D word={hex(v)}\n")
        out = self._run(text)
        self.assertIn("view: blocks=1", out)


if __name__ == "__main__":
    unittest.main()

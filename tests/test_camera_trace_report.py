#!/usr/bin/env python3
"""Contracts for tools/camera_trace_report.py — the GE projection trace reader.

The report must group 0x3E/0x3F commands into 16-lane matrices and decode f24
(the GE's 24-bit float is the top 3 bytes of an IEEE float). A wrong grouping
or decode would silently produce a plausible-but-wrong FOV, which is exactly
the kind of guess the camera trace exists to prevent.
"""
import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import camera_trace_report as report  # noqa: E402


def line(lst, pc, opcode, word):
    return (f"[camera_projection] list={lst} command_pc={pc} "
            f"opcode={opcode} word={word}\n")


def matrix_block(lst, start, words):
    out = line(lst, hex(start), "0x0000003E", "0x3E000000")
    for i, w in enumerate(words):
        out += line(lst, hex(start + 4 + 4 * i), "0x0000003F", hex(0x3F000000 | w))
    return out


class TestF24(unittest.TestCase):
    def test_f24_is_the_top_three_bytes_of_a_float(self):
        # 1/240 as a float is 0x3B888889; its GE f24 argument is 0x3B8888.
        self.assertAlmostEqual(report.f24(0x3B8888), 1.0 / 240.0, places=6)
        self.assertAlmostEqual(report.f24(0x3F8000), 1.0, places=6)
        self.assertAlmostEqual(report.f24(0xBF8000), -1.0, places=6)

    def test_f24_ignores_the_command_byte(self):
        self.assertAlmostEqual(report.f24(0x3F3F8000), 1.0, places=6)


class TestMatrixGrouping(unittest.TestCase):
    def _run(self, text, sites=False):
        out = io.StringIO()
        with redirect_stdout(out):
            report.analyse(Path("/dev/stdin"), sites)
        return out.getvalue()

    def test_a_0x3e_reset_starts_a_new_matrix(self):
        # Feed one complete 16-lane matrix via a temp file.
        import tempfile
        words = [0x3F8000, 0, 0, 0, 0, 0x3F8000, 0, 0,
                 0, 0, 0x3F8000, 0, 0, 0, 0, 0x3F8000]
        text = matrix_block("0x09C51010", 0x9C52238, words)
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
            path = Path(f.name)
        out = io.StringIO()
        with redirect_stdout(out):
            report.analyse(path, True)
        path.unlink()
        text_out = out.getvalue()
        self.assertIn("complete_matrices=1", text_out)
        self.assertIn("distinct_matrices=1", text_out)
        self.assertIn("list=0x09C51010 pc=0x9c52238 count=1", text_out)

    def test_two_different_matrices_are_reported_separately(self):
        import tempfile
        a = [0x3DA8F3] + [0] * 15
        b = [0x3B8888] + [0] * 15
        text = (matrix_block("0x09C51010", 0x9C52238, a) +
                matrix_block("0x09C51010", 0x9C52904, b))
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
            path = Path(f.name)
        out = io.StringIO()
        with redirect_stdout(out):
            report.analyse(path, False)
        path.unlink()
        self.assertIn("complete_matrices=2", out.getvalue())
        self.assertIn("distinct_matrices=2", out.getvalue())

    def test_an_incomplete_block_is_not_counted_as_a_matrix(self):
        import tempfile
        text = line("0x09C51010", "0x9C52238", "0x0000003E", "0x3E000000")
        text += line("0x09C51010", "0x9C5223C", "0x0000003F", "0x3F3F8000")
        with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
            f.write(text)
            path = Path(f.name)
        out = io.StringIO()
        with redirect_stdout(out):
            report.analyse(path, False)
        path.unlink()
        self.assertIn("complete_matrices=0", out.getvalue())


if __name__ == "__main__":
    unittest.main()

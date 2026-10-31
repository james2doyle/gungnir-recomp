#!/usr/bin/env python3
"""T6h — vendored ATRAC3/3+ decoder contracts.

Builds and runs the adapted Nakagawa/FFmpeg n4.4 self-test: create/validation,
garbage rejection, the terminator frame, the IMDCT/IPQF transform canary,
determinism and reset/flush. No game input is used.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "audio" / "atrac3p_selftest"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "atrac3p_selftest"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"atrac3p self-test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestAtracDecoder(unittest.TestCase):
    def test_public_selftest(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"ATRAC decoder self-test failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("all", proc.stdout)
        self.assertIn("checks passed", proc.stdout)

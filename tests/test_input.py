#!/usr/bin/env python3
"""T11a — input core and sceCtrl mapping host gate.

Builds the source-neutral input tests and runs the PSP button/analog mapping,
source-merge, edge and desktop key-map contracts.

Red-first: written before runtime/input existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "input_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "input_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"input tests build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestInputCore(unittest.TestCase):
    def test_mapping_merge_edges_and_key_map(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"input contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("input_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

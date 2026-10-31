#!/usr/bin/env python3
"""T7a — GE display-list decoder contracts.

Builds and runs the synthetic-word fixture in tests/ge/display_list_tests.cpp.
No game data is required.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "ge" / "ge_display_list_tests"
VERTEX_BINARY = BUILD / "tests" / "ge" / "ge_vertex_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(
        ["cmake", "--build", str(BUILD), "--target", "ge_display_list_tests", "ge_vertex_tests"],
        capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"GE test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestGeDisplayList(unittest.TestCase):
    def test_decoder_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0,
                         f"GE decoder contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("ge_display_list_tests: OK", proc.stdout)

    def test_vertex_contracts(self):
        proc = subprocess.run([str(VERTEX_BINARY)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0,
                         f"GE vertex contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("ge_vertex_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

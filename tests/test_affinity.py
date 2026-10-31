#!/usr/bin/env python3
"""T9e — CPU-list parsing and best-effort affinity/priority contracts."""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "platform" / "affinity_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "affinity_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"affinity test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestAffinity(unittest.TestCase):
    def test_affinity_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"affinity contracts failed:\n{output}")
        self.assertIn("affinity_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

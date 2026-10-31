#!/usr/bin/env python3
"""T8a — TPL container decode contracts.

The C++ fixture builds synthetic TPL containers in memory; no game bytes are
read or committed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "tex" / "tex_tpl_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "tex_tpl_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"TPL test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestTplDecode(unittest.TestCase):
    def test_tpl_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"TPL contracts failed:\n{output}")
        self.assertIn("tex_tpl_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T8d — replacement lookup, precedence, scaling and fallback.

Builds the replacement fixture when libpng is available; otherwise reports a
named skip.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "tex" / "tex_replace_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise unittest.SkipTest(f"cmake configure failed: {cfg.stderr[-1000:]}")
    targets = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                             capture_output=True, text=True)
    if "tex_replace_tests" not in targets.stdout:
        raise unittest.SkipTest("tex_replace_tests not built (libpng missing)")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "tex_replace_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"replacement test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestTextureReplacement(unittest.TestCase):
    def test_replacement_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"replacement contracts failed:\n{output}")
        self.assertIn("tex_replace_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

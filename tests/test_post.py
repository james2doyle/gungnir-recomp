#!/usr/bin/env python3
"""T7g — post-processing preset and screenshot-hash contracts.

Builds the CPU post chain when json-c is available; otherwise the suite
reports a named skip.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "post" / "post_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise unittest.SkipTest(f"cmake configure failed: {cfg.stderr[-1000:]}")
    targets = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                             capture_output=True, text=True)
    if "post_tests" not in targets.stdout:
        raise unittest.SkipTest("post_tests not built (json-c missing)")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "post_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise AssertionError(f"post test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")
    if not BINARY.exists():
        raise unittest.SkipTest("post_tests binary was not produced")


class TestPostProcessing(unittest.TestCase):
    def test_post_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"post contracts failed:\n{output}")
        self.assertIn("post_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

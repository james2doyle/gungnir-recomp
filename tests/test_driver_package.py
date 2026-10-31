#!/usr/bin/env python3
"""T10b — untrusted driver zip inspection, install and removal contracts."""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "platform" / "driver_package_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "driver_package_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"driver package test build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestDriverPackage(unittest.TestCase):
    def test_package_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"driver package contracts failed:\n{output}")
        self.assertIn("driver_package_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

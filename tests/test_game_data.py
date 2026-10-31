#!/usr/bin/env python3
"""T9c — game-data discovery, identity gate and asset-stamp contracts."""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "platform" / "game_data_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "game_data_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"game-data test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestGameDataIntake(unittest.TestCase):
    def test_intake_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"game-data contracts failed:\n{output}")
        self.assertIn("game_data_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

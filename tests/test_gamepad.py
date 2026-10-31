#!/usr/bin/env python3
"""T11d — gamepad profile/state/remap host gate.

Builds the C++ profile, shipped-match, state and remap contracts.

Red-first: written before runtime/input/gamepad_* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "gamepad_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "gamepad_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"gamepad tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestGamepadCore(unittest.TestCase):
    def test_profiles_state_and_remap(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"gamepad contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("gamepad_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

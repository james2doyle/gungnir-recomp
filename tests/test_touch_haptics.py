#!/usr/bin/env python3
"""T11c — touch-haptics policy host gate.

Builds the C++ policy tests: one tick per button/d-pad down, none for the
stick/edit/move/release, disabled or absent sink no-op, rapid presses survive,
and a throwing sink is contained.

Red-first: written before runtime/input/touch_haptics.* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "touch_haptics_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "touch_haptics_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"touch haptics tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestTouchHaptics(unittest.TestCase):
    def test_policy_and_failure_paths(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"touch haptics contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("touch_haptics_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

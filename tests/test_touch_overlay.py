#!/usr/bin/env python3
"""T11b — touch overlay layout, pointer and JSON host gate.

Builds the C++ touch-overlay model tests: default layout, safe-area mapping,
multi-pointer hit-testing, floating stick, d-pad, edit clamping and the
versioned JSON schema.

Red-first: written before runtime/input/touch_overlay.* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "touch_overlay_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "touch_overlay_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"touch overlay tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestTouchOverlay(unittest.TestCase):
    def test_layout_pointer_edit_and_json(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"touch overlay contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("touch_overlay_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

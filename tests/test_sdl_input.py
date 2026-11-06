#!/usr/bin/env python3
"""SdlInput touch-mouse and system-back host gate.

Skipped with a named reason when the host has no SDL3 pkg-config, because the
adapter is only built in that configuration.

Red-first: written for the Odin3 phantom-Cross (synthetic touch-mouse mapped
to Cross) and the dead system-back key (SDL-consumed AC_BACK dropped).
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "sdl_input_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "sdl_input_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        if "unknown target" in (bld.stdout + bld.stderr).lower():
            raise unittest.SkipTest("SDL3 pkg-config not available on this host")
        raise RuntimeError(f"SDL input tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSdlInput(unittest.TestCase):
    def test_touch_mouse_ignored_and_back_toggles_menu(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"SDL input contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("sdl_input_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

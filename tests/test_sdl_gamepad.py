#!/usr/bin/env python3
"""T11d — SDL gamepad adapter host gate with a virtual joystick.

Skipped with a named reason when the host has no SDL3 pkg-config, because the
adapter is only built in that configuration.

Red-first: written before runtime/platform/sdl_gamepad.* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "sdl_gamepad_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "sdl_gamepad_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        if "unknown target" in (bld.stdout + bld.stderr).lower():
            raise unittest.SkipTest("SDL3 pkg-config not available on this host")
        raise RuntimeError(f"SDL gamepad tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSdlGamepad(unittest.TestCase):
    def test_virtual_hotplug_mapping_and_disconnect(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"SDL gamepad contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("sdl_gamepad_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

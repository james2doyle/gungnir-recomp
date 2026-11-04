#!/usr/bin/env python3
"""Fast-forward host-control gate.

Builds the pure host-control model and runs the hold/latch/speed contracts it
shares with the gamepad adapter, the touch overlay and the kernel pacing hold.

Red-first: written before runtime/input/host_controls.* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "host_controls_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "host_controls_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"host control tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestHostControls(unittest.TestCase):
    def test_hold_latch_and_speed(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"host control contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("host_controls_tests: OK", proc.stdout)

    def test_pacing_hold_uses_the_control_speed(self):
        """The kernel must hold emulated time to real time at the control's
        multiple; a nominal-only hold would leave fast-forward at 1x."""
        header = (REPO / "runtime" / "hle" / "kernel.hpp").read_text()
        session = (REPO / "runtime" / "session" / "game_session.cpp").read_text()
        self.assertIn("set_speed_provider", header,
                      "the kernel takes the speed from a host provider")
        self.assertIn("host_controls().speed()", session,
                      "the session wires the host speed into the kernel")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Startup limits — the runtime must not spend the shell's stack.

Red-first context: the game-data identity gate kept a 1 MiB scratch buffer as
a local, which is the whole of the SDL thread's stack, so the app crashed at
startup only on the device and only sometimes. These contracts keep the cause
fixed: a bounded frame budget in the build, an identity gate that fits a small
thread, a measurable stack margin, and a repeated cold-boot gate.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARIES = {
    "game_data_tests": BUILD / "tests" / "platform" / "game_data_tests",
    "stack_space_tests": BUILD / "tests" / "platform" / "stack_space_tests",
}


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    bld = subprocess.run(
        ["cmake", "--build", str(BUILD), "--target", *BINARIES],
        capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"startup-limit test build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestStartupLimits(unittest.TestCase):
    def test_identity_gate_and_stack_probe(self):
        for name, binary in BINARIES.items():
            proc = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120)
            output = proc.stdout + proc.stderr
            self.assertEqual(proc.returncode, 0, f"{name} failed:\n{output}")
            self.assertIn(f"{name}: OK", proc.stdout)

    def test_frame_budget_is_enforced_in_both_build_roots(self):
        helper = REPO / "cmake" / "BoundedStack.cmake"
        self.assertTrue(helper.is_file(), "the frame budget helper exists")
        text = helper.read_text()
        self.assertIn("Werror=frame-larger-than=", text, "the bound fails the build")
        self.assertIn("GUNGNIR_MAX_STACK_FRAME", text, "the bound is configurable")
        host = (REPO / "CMakeLists.txt").read_text()
        self.assertIn("include(cmake/BoundedStack.cmake)", host)
        self.assertIn("gungnir_bound_stack_frames(", host)
        for target in ("gungnir_input", "gungnir_hle", "gungnir_ge", "gungnir_tex",
                       "gungnir_platform", "gungnir_adreno"):
            self.assertIn(target, host, f"{target} is bounded")
        android = (REPO / "cmake" / "AndroidRuntime.cmake").read_text()
        self.assertIn("BoundedStack.cmake", android, "the shell build shares the bound")
        self.assertIn("gungnir_bound_stack_frames(${target})", android)

    def test_runtime_reports_its_stack_margin(self):
        header = (REPO / "runtime" / "platform" / "android" / "stack_space.hpp").read_text()
        self.assertIn("struct StackSpace", header)
        self.assertIn("headroom_bytes", header, "the margin is reported, not assumed")
        main = (REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp").read_text()
        self.assertIn('log_stack_space("main")', main, "the SDL thread reports first")
        self.assertIn('log_stack_space("session")', main, "the guest thread reports too")
        runtime = (REPO / "cmake" / "AndroidRuntime.cmake").read_text()
        self.assertIn("runtime/platform/android/stack_space.cpp", runtime)

    def test_cold_boot_gate_repeats_and_fails_on_crashes(self):
        script = REPO / "scripts" / "android" / "qa-startup.sh"
        self.assertTrue(script.is_file(), "the cold-boot gate exists")
        self.assertTrue(script.stat().st_mode & 0o111, "the cold-boot gate is executable")
        text = script.read_text()
        self.assertIn("RUNS=${RUNS:-10}", text, "ten boots by default")
        self.assertIn("Fatal signal", text, "a crash fails the gate")
        self.assertIn("first frame", text, "a boot proves itself with a presented frame")
        self.assertIn("exit 1", text, "failures are not swallowed")


if __name__ == "__main__":
    unittest.main()

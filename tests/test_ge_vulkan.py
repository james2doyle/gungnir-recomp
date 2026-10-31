#!/usr/bin/env python3
"""T7e — Vulkan GE backend contracts.

Builds and runs the C++ Vulkan fixture when the host has a Vulkan loader,
headers and a shader compiler. Otherwise the suite reports a named skip.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "ge" / "ge_vulkan_tests"


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise unittest.SkipTest(f"cmake configure failed: {cfg.stderr[-1000:]}")
    targets = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                             capture_output=True, text=True)
    if "ge_vulkan_tests" not in targets.stdout:
        raise unittest.SkipTest("ge_vulkan_tests not built (Vulkan loader, headers or glslc missing)")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "ge_vulkan_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise AssertionError(f"Vulkan test build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")
    if not BINARY.exists():
        raise unittest.SkipTest("ge_vulkan_tests binary was not produced")


class TestGeVulkan(unittest.TestCase):
    def test_vulkan_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        if proc.returncode == 77 or "SKIP:" in output:
            self.skipTest(output.strip())
        self.assertEqual(proc.returncode, 0, f"Vulkan contracts failed:\n{output}")
        self.assertIn("ge_vulkan_tests: OK", output)


if __name__ == "__main__":
    unittest.main()

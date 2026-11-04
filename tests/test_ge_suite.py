#!/usr/bin/env python3
"""T7h — consolidated GE fixture gate (TC-7.1–TC-7.4).

Runs the decoder, vertex, texture, software, Vulkan and post fixture binaries
as one suite. Vulkan is skipped by name when the host cannot build or run it.
"""
import subprocess
import unittest
from pathlib import Path

from tests import suite_support

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
GE = BUILD / "tests" / "ge"
POST = BUILD / "tests" / "post"

BINARIES = {
    "decoder": GE / "ge_display_list_tests",
    "pacing": GE / "frame_pacing_tests",
    "identity": GE / "draw_identity_tests",
    "vertex": GE / "ge_vertex_tests",
    "texture": GE / "ge_texture_tests",
    "software": GE / "ge_software_tests",
    "vulkan": GE / "ge_vulkan_tests",
    "post": POST / "post_tests",
}


CHILD_MODULES = ("tests.test_ge", "tests.test_ge_vulkan", "tests.test_post")


def setUpModule():
    if not suite_support.missing(CHILD_MODULES):
        return
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    available = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                               capture_output=True, text=True).stdout
    targets = ["ge_display_list_tests", "ge_vertex_tests", "ge_texture_tests", "ge_software_tests"]
    if "ge_vulkan_tests" in available:
        targets.append("ge_vulkan_tests")
    if "post_tests" in available:
        targets.append("post_tests")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", *targets],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"GE suite build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestGeFixtureSuite(unittest.TestCase):
    def run_binary(self, name, timeout=120):
        binary = BINARIES[name]
        if not binary.exists():
            self.skipTest(f"{name} fixture binary is not built")
        proc = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
        output = proc.stdout + proc.stderr
        if proc.returncode == 77 or "SKIP:" in output:
            self.skipTest(output.strip())
        self.assertEqual(proc.returncode, 0, f"{name} failed:\n{output}")
        self.assertIn("OK", output)
        return output

    def test_tc71_ge_primitives_state_and_textures(self):
        if not suite_support.missing(("tests.test_ge",)):
            self.skipTest("tests.test_ge is discovered separately")
        for name in ("decoder", "vertex", "texture", "software"):
            self.run_binary(name)

    def test_tc72_software_vs_vulkan_parity(self):
        if not suite_support.missing(("tests.test_ge_vulkan",)):
            self.skipTest("tests.test_ge_vulkan is discovered separately")
        self.run_binary("vulkan")

    def test_tc73_post_preset_hashes(self):
        if not suite_support.missing(("tests.test_post",)):
            self.skipTest("tests.test_post is discovered separately")
        self.run_binary("post")

    def test_tc74_feedback_regression(self):
        missing = suite_support.missing(("tests.test_ge", "tests.test_ge_vulkan"))
        if not missing:
            self.skipTest("GE and Vulkan modules are discovered separately")
        if "tests.test_ge" in missing:
            self.run_binary("texture")
        if "tests.test_ge_vulkan" in missing:
            self.run_binary("vulkan")

    def test_renderer_and_shader_docs(self):
        renderer = (REPO / "docs" / "RENDERER.md").read_text()
        shaders = (REPO / "docs" / "SHADERS.md").read_text()
        self.assertIn("TC-7.1", renderer)
        self.assertIn("TC-7.2", renderer)
        self.assertIn("TC-7.3", renderer)
        self.assertIn("TC-7.4", renderer)
        self.assertIn("named gaps", renderer)
        self.assertIn("Preset JSON", shaders)


if __name__ == "__main__":
    unittest.main()

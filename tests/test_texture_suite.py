#!/usr/bin/env python3
"""T8f — consolidated texture pipeline gate (TC-8.1–TC-8.4)."""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
GE = BUILD / "tests" / "tex"

BINARIES = {
    "tex_tpl_tests": GE / "tex_tpl_tests",
    "tex_dump_tests": GE / "tex_dump_tests",
    "tex_replace_tests": GE / "tex_replace_tests",
}


def setUpModule():
    cfg = subprocess.run(
        ["cmake", "-S", str(REPO), "-B", str(BUILD), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
        capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr[-4000:]}")
    available = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                               capture_output=True, text=True).stdout
    targets = [name for name in BINARIES if name in available]
    if targets:
        bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", *targets],
                             capture_output=True, text=True)
        if bld.returncode != 0:
            raise RuntimeError(f"texture suite build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestTextureSuite(unittest.TestCase):
    def run_binary(self, name, timeout=60):
        binary = BINARIES[name]
        if not binary.exists():
            self.skipTest(f"{name} not built (libpng missing)")
        proc = subprocess.run([str(binary)], capture_output=True, text=True, timeout=timeout)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"{name} failed:\n{output}")
        self.assertIn("OK", proc.stdout)

    def test_tc81_tpl_round_trip(self):
        self.run_binary("tex_tpl_tests")

    def test_tc82_dump_replace_reload(self):
        self.run_binary("tex_dump_tests")
        self.run_binary("tex_replace_tests")

    def test_tc83_font_atlas(self):
        proc = subprocess.run([sys.executable, "-m", "unittest", "tests.test_font_repack"],
                              cwd=REPO, capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"font atlas contracts failed:\n{output}")

    def test_tc84_swizzle_edge_cases(self):
        self.run_binary("tex_tpl_tests")

    def test_docs_and_mutation_ledger(self):
        tpl = (REPO / "docs" / "formats" / "TPL.md").read_text()
        dar = (REPO / "docs" / "formats" / "DAR.md").read_text()
        replacement = (REPO / "docs" / "TEXTURE_REPLACEMENT.md").read_text()
        for gate in ("TC-8.1", "TC-8.2", "TC-8.3", "TC-8.4"):
            self.assertIn(gate, replacement)
        self.assertIn("Mutation evidence", replacement)
        self.assertIn("Named gaps", replacement)
        self.assertIn("stored_size == 0", dar)
        self.assertIn("byte-identical", tpl)


if __name__ == "__main__":
    unittest.main()

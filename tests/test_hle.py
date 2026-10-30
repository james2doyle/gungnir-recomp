#!/usr/bin/env python3
"""T6a — HLE registrar and fail-closed unknown-NID tests.

The C++ fixture loads configs/nids.csv, binds handlers by (library, name), and
proves that an unbound import stops with a named NID instead of returning a
silent zero.

Red-first: written before runtime/hle or the hle_import_tests target existed.
"""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "hle" / "hle_import_tests"
NIDS = REPO / "configs" / "nids.csv"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "hle_import_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"hle_import_tests build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestHleRegistry(unittest.TestCase):
    def run_case(self, *args):
        proc = subprocess.run([str(BINARY), "--nids", str(NIDS), *args],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0,
                         f"case {args} failed:\n{proc.stdout}{proc.stderr}")
        return proc.stdout + proc.stderr

    def test_csv_resolves_cross_library_alias(self):
        text = self.run_case("--csv")
        self.assertIn("Kernel_Library:0x293B45B8=sceKernelGetThreadId", text)

    def test_registrar_binds_by_name(self):
        self.run_case("--registrar")

    def test_unknown_import_is_loud_and_named(self):
        text = self.run_case("--unknown")
        self.assertIn("Missing HLE import", text)
        self.assertIn("NID_0xDEADBEEF", text)
        self.assertIn("TestLibrary", text)

    def test_unknown_nid_does_not_return_zero(self):
        text = self.run_case("--unknown")
        self.assertIn("stopped", text)
        self.assertNotIn("silent-zero", text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T12b — savedata store, PARAM.SFO and PPSSPP interchange contracts.

Red-first: the store, SFO builder and HLE load/save paths did not exist when
this test was written. The exported PPSSPP memstick saves are user-local
under gitignored savestates/; when absent the fixture case skips with the
named reason savedata_fixture_missing.
"""
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
SAVEDATA_BINARY = BUILD / "tests" / "savedata" / "savedata_tests"
KERNEL_BINARY = BUILD / "tests" / "hle" / "hle_kernel_tests"
NIDS = REPO / "configs" / "nids.csv"
GAME_ISO = REPO / "game.iso"
FIXTURES = REPO / "savestates"
EXPORTED = (FIXTURES / "ULUS10592GUN9THSYS", FIXTURES / "ULUS10592GUN9THSTORY00")


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD),
                          "--target", "savedata_tests", "hle_kernel_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"savedata tests build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSavedataStore(unittest.TestCase):
    def test_store_and_sfo_contracts(self):
        with tempfile.TemporaryDirectory(prefix="gungnir-savedata-") as scratch:
            proc = subprocess.run([str(SAVEDATA_BINARY), "--scratch", scratch],
                                  capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"savedata store contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("savedata_tests: OK", proc.stdout)

    def test_exported_ppsspp_memstick_saves(self):
        missing = [path.name for path in EXPORTED if not path.is_dir()]
        if missing:
            self.skipTest(f"savedata_fixture_missing: {', '.join(missing)}")
        with tempfile.TemporaryDirectory(prefix="gungnir-savedata-") as scratch:
            proc = subprocess.run([str(SAVEDATA_BINARY), "--fixtures", str(FIXTURES),
                                   "--scratch", scratch],
                                  capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"exported save contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("savedata_tests: OK", proc.stdout)


class TestSavedataHle(unittest.TestCase):
    def test_save_load_round_trip(self):
        args = [str(KERNEL_BINARY), "--nids", str(NIDS)]
        with tempfile.TemporaryDirectory(prefix="gungnir-ms-") as ms:
            if GAME_ISO.exists():
                args += ["--iso", str(GAME_ISO)]
            args += ["--ms", ms]
            proc = subprocess.run(args, capture_output=True, text=True, timeout=180)
        self.assertEqual(proc.returncode, 0,
                         f"HLE savedata contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("hle_kernel_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

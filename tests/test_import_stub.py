#!/usr/bin/env python3
"""k9ll558m — interpreter-only import dispatch regression.

The C++ fixture runs guest code that calls a tail-stub import: without the
import-stub trampoline the interpreter executes the stub and returns without
calling the HLE (the module_start no-op bug that parked the interpreter-only
build); with it the HLE runs and the guest continues. A pre-registered
function at the stub (the AOT build case) must survive registration.

Red-first: written after the device stall was traced to stub code running
instead of dispatching; the green case fails until register_import_stub_
trampolines exists and is called from GameSession::start.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "session" / "import_stub_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD),
                          "--target", "import_stub_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"import_stub_tests build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestImportStubDispatch(unittest.TestCase):
    def test_trampoline_contract(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0,
                         f"import_stub_tests failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("import_stub_tests: OK", proc.stdout)

    def test_session_wires_the_trampoline(self):
        text = (REPO / "runtime" / "session" / "game_session.cpp").read_text()
        self.assertIn("register_import_stub_trampolines", text,
                      "GameSession::start must register import-stub trampolines")


if __name__ == "__main__":
    unittest.main()

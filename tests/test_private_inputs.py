#!/usr/bin/env python3
"""T13d — fail-closed private-input contract (red-first).

Private-input tests must skip cleanly on a developer box but can never
pass silently in CI: with GUNGNIR_REQUIRE_PRIVATE_INPUT=1 a missing input
is a hard failure (or process exit 77 where ctest maps the skip).
"""
import os
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HELPER = REPO / "tests" / "private_inputs.py"
PROBE = REPO / "tests" / "private_input_probe.py"


def run_probe(strict):
    env = dict(os.environ)
    env["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = "1" if strict else "0"
    return subprocess.run([sys.executable, str(PROBE)], capture_output=True, text=True, env=env)


class TestPrivateInputs(unittest.TestCase):
    def test_helper_exists(self):
        self.assertTrue(HELPER.is_file(), "tests/private_inputs.py missing")

    def test_non_strict_skips_cleanly(self):
        sys.path.insert(0, str(REPO / "tests"))
        try:
            import private_inputs
        except ImportError as exc:
            self.fail(f"private_inputs unimportable: {exc}")
        env = dict(os.environ)
        env["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = "0"
        old = os.environ.get("GUNGNIR_REQUIRE_PRIVATE_INPUT")
        os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = "0"
        try:
            with self.assertRaises(unittest.SkipTest):
                private_inputs.require_private_input("unit fixture")
        finally:
            if old is None:
                del os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"]
            else:
                os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = old
        _ = env

    def test_strict_mode_fails_closed(self):
        sys.path.insert(0, str(REPO / "tests"))
        try:
            import private_inputs
        except ImportError as exc:
            self.fail(f"private_inputs unimportable: {exc}")
        old = os.environ.get("GUNGNIR_REQUIRE_PRIVATE_INPUT")
        os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = "1"
        try:
            with self.assertRaises(AssertionError):
                private_inputs.require_private_input("unit fixture")
        finally:
            if old is None:
                del os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"]
            else:
                os.environ["GUNGNIR_REQUIRE_PRIVATE_INPUT"] = old

    def test_probe_exit_codes(self):
        self.assertTrue(PROBE.is_file(), "tests/private_input_probe.py missing")
        sys.path.insert(0, str(REPO / "tests"))
        try:
            import private_input_probe
        except ImportError as exc:
            self.fail(f"probe unimportable: {exc}")
        self.assertEqual(private_input_probe.decide(True), 0, "present input must exit 0")
        self.assertEqual(private_input_probe.decide(False), 77, "absent input must exit 77, never pass")
        proc = run_probe(strict=False)
        strict_proc = run_probe(strict=True)
        self.assertEqual(proc.returncode, strict_proc.returncode,
                         "strict mode must not change the presence report")
        self.assertIn(proc.returncode, (0, 77), f"unexpected probe code {proc.returncode}")

    def test_cmake_maps_exit_77(self):
        text = (REPO / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("SKIP_RETURN_CODE 77", text, "CMake never maps exit 77")


if __name__ == "__main__":
    unittest.main()

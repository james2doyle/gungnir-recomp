#!/usr/bin/env python3
"""T11e — consolidated input/haptics gate and docs contract.

Runs the T11a-T11d host gates and checks that docs/INPUT.md documents the
merge policy, overlay/editor JSON, keyboard map, gamepad profiles/remap,
touch-haptics behavior and the named gaps.
"""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOC = REPO / "docs" / "INPUT.md"

MODULES = (
    "tests.test_input",
    "tests.test_touch_overlay",
    "tests.test_touch_haptics",
    "tests.test_gamepad",
    "tests.test_sdl_gamepad",
)


class TestInputHostGate(unittest.TestCase):
    def test_host_modules(self):
        proc = subprocess.run([sys.executable, "-m", "unittest", *MODULES],
                              cwd=REPO, capture_output=True, text=True, timeout=900)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"input host gates failed:\n{output}")

    def test_docs_cover_the_input_path(self):
        self.assertTrue(DOC.is_file(), "docs/INPUT.md exists")
        text = DOC.read_text()
        for heading in ("## Source merge", "## Touch overlay", "## Gamepad",
                        "## Remap", "## Keyboard and mouse", "## Touch haptics",
                        "## Named gaps"):
            self.assertIn(heading, text, f"missing section: {heading}")
        for term in ("HAPTIC_FEEDBACK_ENABLED", "profiles.json", "versioned JSON",
                     "touch_layout_corrupt_default", "Push the left stick", "T13"):
            self.assertIn(term, text, f"missing doc contract: {term}")


if __name__ == "__main__":
    unittest.main()

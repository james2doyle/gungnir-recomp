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

from tests import suite_support

REPO = Path(__file__).resolve().parent.parent
DOC = REPO / "docs" / "INPUT.md"

MODULES = (
    "tests.test_input",
    "tests.test_touch_overlay",
    "tests.test_touch_haptics",
    "tests.test_gamepad",
    "tests.test_sdl_gamepad",
    "tests.test_menu_navigation",
)


class TestInputHostGate(unittest.TestCase):
    def test_host_modules(self):
        missing = suite_support.missing(MODULES)
        if not missing:
            self.skipTest("child modules are discovered separately")
        proc = subprocess.run([sys.executable, "-m", "unittest", *missing],
                              cwd=REPO, capture_output=True, text=True, timeout=900)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"input host gates failed:\n{output}")

    def test_unmapped_pad_toast_wired(self):
        bridge = (REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp"
                  / "input" / "GamepadBridge.java").read_text()
        overlay = (REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp"
                   / "input" / "TouchOverlayView.java").read_text()
        native = (REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp").read_text()
        gamepad = (REPO / "runtime" / "platform" / "sdl_gamepad.cpp").read_text()
        self.assertIn("needsRemap", bridge, "GamepadBridge exposes the remap-needed flag")
        self.assertIn("GamepadBridge_needsRemap", native, "native layer answers needsRemap")
        self.assertIn("GetGamepadMappingForGUID", gamepad,
                      "unmapped detection uses the SDL mapping for the GUID")
        self.assertIn("remap", overlay.lower(), "overlay poll handles the unmapped case")
        self.assertIn("Menu > Gamepad", overlay, "toast points at the remap page")
        self.assertIn("gamecontrollerdb", DOC.read_text().lower(),
                      "docs explain the SDL-mapping gap behind the toast")

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

#!/usr/bin/env python3
"""Screen-fill presentation: fit / stretch / crop surfaces.

Presentation only — the guest still renders 480x272. The geometry contracts
live in `tests/ge/vulkan_tests.cpp` (red-first: `present_rect` and the scissor
clamp did not exist when they were written); this module holds the shell
surfaces together so the option cannot silently lose its intent extra, its
persistence or its snapshot purity.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
BRIDGE = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "input" / "HostBridge.java"
OVERLAY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "SettingsOverlayView.java"
ACTIVITY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "GungnirActivity.java"
SNAPSHOT_SOURCES = ("runtime/session/snapshot.cpp", "runtime/session/game_session.cpp")
RUNTIME = REPO / "runtime" / "ge" / "vulkan_backend.cpp"


class TestDisplayLayoutSurfaces(unittest.TestCase):
    def test_presentation_policy_is_named(self):
        text = (REPO / "runtime" / "ge" / "vulkan_backend.cpp").read_text()
        for policy in ("DisplayLayout::Fit", "DisplayLayout::Stretch", "DisplayLayout::Crop"):
            self.assertIn(policy, text, f"{policy} is implemented")
        self.assertIn('"unknown_display_layout: "', text,
                      "an unknown policy is named, never guessed")
        self.assertIn("clamp_scissor_to_target", text,
                      "crop clamps its scissor to the surface")

    def test_shell_exposes_the_policy(self):
        main = MAIN_CPP.read_text()
        self.assertIn('intent_string("display_layout")', main, "QA can pin the layout")
        self.assertIn('GUNGNIR_LOGI("display_layout=%s', main, "the active layout is logged")
        self.assertIn("Java_com_gungnir_recomp_input_HostBridge_nativeSetDisplayLayout", main)
        self.assertIn("Java_com_gungnir_recomp_input_HostBridge_displayLayout", main)
        self.assertIn("backend.set_display_layout(", main, "the backend follows the host policy")
        bridge = BRIDGE.read_text()
        self.assertIn("nativeSetDisplayLayout", bridge)
        self.assertIn("String displayLayout()", bridge)

    def test_settings_row_cycles_and_persists(self):
        overlay = OVERLAY.read_text()
        self.assertIn("Screen fill: ", overlay, "the row names the active policy")
        self.assertIn("onCycleDisplayLayout", overlay)
        activity = ACTIVITY.read_text()
        self.assertIn('putString("display_layout"', activity, "the policy persists")
        self.assertIn('getString("display_layout", "fit")', activity, "fit is the default")
        self.assertIn('getIntent().hasExtra("display_layout")', activity,
                      "the QA intent wins over the persisted setting")

    def test_presentation_never_enters_a_snapshot(self):
        for relative in SNAPSHOT_SOURCES:
            text = (REPO / relative).read_text()
            self.assertNotIn("display_layout", text, f"{relative} records presentation")
            self.assertNotIn("DisplayLayout", text, f"{relative} records presentation")


if __name__ == "__main__":
    unittest.main()

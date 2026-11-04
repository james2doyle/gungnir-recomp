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
VERTEX = REPO / "runtime" / "ge" / "vertex.cpp"
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

    def test_layout_is_not_a_dead_settings_row(self):
        # While adaptive widescreen fills the display the three policies
        # coincide, so offering the row would be a control that does nothing.
        overlay = OVERLAY.read_text()
        self.assertNotIn("Screen fill: ", overlay, "no dead presentation row")
        self.assertNotIn("onCycleDisplayLayout", overlay)
        activity = ACTIVITY.read_text()
        self.assertIn('hasExtra("display_layout")', activity,
                      "the QA intent still reaches the renderer")
        self.assertIn('getString("display_layout", "fit")', activity,
                      "the persisted policy is still applied")

    def test_geometry_contracts_exist(self):
        text = VERTEX.read_text()
        self.assertIn("bool covers_guest_frame(", text,
                      "full-frame detection is a tested unit")
        self.assertIn("float wide_screen_x(", text, "the mapping is a tested unit")
        self.assertIn("world_view_projection_.m[0] *= inverse", text,
                      "the frustum widens through the matrix row")

    def test_widescreen_follows_the_window(self):
        main = MAIN_CPP.read_text()
        self.assertIn('intent_string("widescreen")', main, "QA can pin the mode")
        self.assertIn('mode.empty() ? "adaptive"', main,
                      "adaptive is the default: the frame follows the window")
        for cap in ('"16:9"', '"21:9"', '"32:9"', '"off"'):
            self.assertIn(cap, main, f"{cap} is an accepted cap")
        self.assertIn("unknown_widescreen: %s", main, "an unknown mode is named")
        self.assertIn("gungnir::hle::set_widescreen_scale", main,
                      "the scene pipeline learns the factor")
        self.assertIn("covers_guest_frame", main,
                      "full-frame 2D stretches with the scene")
        self.assertIn('GUNGNIR_LOGI("widescreen mode=%s scale=%.4f', main,
                      "the applied factor is logged, never assumed")

    def test_presentation_never_enters_a_snapshot(self):
        for relative in SNAPSHOT_SOURCES:
            text = (REPO / relative).read_text()
            self.assertNotIn("display_layout", text, f"{relative} records presentation")
            self.assertNotIn("DisplayLayout", text, f"{relative} records presentation")


if __name__ == "__main__":
    unittest.main()

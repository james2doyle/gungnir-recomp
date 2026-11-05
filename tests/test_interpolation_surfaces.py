#!/usr/bin/env python3
"""T17a.7 — interpolation option, diagnostics and snapshot contracts.

Interpolation is host presentation policy: it must be off by default, switchable
from the settings overlay, persisted, and invisible to a snapshot. Its history
is derived, so every path that breaks continuity (snapshot load, reset to
title, background resume) must drop it.

Red-first: written before the option and its invalidation existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
BRIDGE = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "input" / "HostBridge.java"
OVERLAY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "SettingsOverlayView.java"
ACTIVITY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "GungnirActivity.java"
SNAPSHOT_SOURCES = ("runtime/session/snapshot.cpp", "runtime/session/game_session.cpp")


class TestInterpolationOption(unittest.TestCase):
    def test_settings_row_toggles_and_persists(self):
        overlay = OVERLAY.read_text()
        self.assertIn("Experimental frame interpolation: Off", overlay,
                      "the settings overlay must offer the toggle, labelled as experimental")
        self.assertIn("setFrameInterpolation", overlay,
                      "the row label must reflect the current state")
        activity = ACTIVITY.read_text()
        for needle in ("onToggleFrameInterpolation", "frame_interpolation",
                       "nativeSetFrameInterpolation", "Experimental frame interpolation on"):
            self.assertIn(needle, activity, f"activity is missing {needle}")

    def test_native_bridge_exposes_the_toggle(self):
        bridge = BRIDGE.read_text()
        self.assertIn("nativeSetFrameInterpolation", bridge)
        self.assertIn("frameInterpolation", bridge)
        main = MAIN_CPP.read_text()
        self.assertIn("Java_com_gungnir_recomp_input_HostBridge_nativeSetFrameInterpolation", main)
        self.assertIn("Java_com_gungnir_recomp_input_HostBridge_frameInterpolation", main)

    def test_defaults_off_and_has_a_qa_intent(self):
        main = MAIN_CPP.read_text()
        self.assertIn("std::atomic<bool> g_interpolation_enabled{false};", main,
                      "interpolation must default to off")
        self.assertIn('intent_long("frame_interpolation"', main,
                      "QA needs an intent extra to A/B a bounded run")

    def test_diagnostics_name_the_mode(self):
        main = MAIN_CPP.read_text()
        self.assertIn("interpolation active=", main,
                      "the periodic log must say whether in-betweens are presented")
        self.assertIn("skipped_ticks=", main,
                      "missing refreshes must be visible, not silent")

    def test_guest_frame_log_reads_the_stored_frame(self):
        # The guest draw list is moved into the interpolation state, so logging
        # or counting `guest_draws` afterwards reads a moved-from vector: the
        # whole block was dead and the counters never advanced. Named here so it
        # cannot come back as a silent gap.
        main = MAIN_CPP.read_text()
        self.assertIn("const std::vector<gungnir::ge::Draw> &frame_draws = g_interpolation.current;",
                      main)
        self.assertNotIn("guest_draws.size()", main,
                         "guest_draws is moved from before the log block; use frame_draws")

    def test_pair_diagnostics_report_what_the_pass_did(self):
        main = MAIN_CPP.read_text()
        for needle in ("interpolation_pairs", "reassigned=", "displaced=", "motion=",
                       "oversize_draws=", "translated=", "largest_bucket="):
            self.assertIn(needle, main, f"pair diagnostics are missing {needle}")
        for needle in ("timeline presents=", "regressions=", "per_frame="):
            self.assertIn(needle, main, f"timeline diagnostics are missing {needle}")

    def test_draw_dump_is_bounded_and_named(self):
        main = MAIN_CPP.read_text()
        self.assertIn('intent_string("dump_draws")', main,
                      "presentation work needs a dump of real guest frames")
        self.assertIn('intent_long("dump_after_ms"', main,
                      "the dump must be able to start after the scene has settled")
        self.assertIn("std::uint32_t limit{8u};", main,
                      "the dump must be bounded, never an unbounded session recording")
        self.assertIn('"draw_dump frames=%u', main,
                      "a dump that ends must say so, with its path")

    def test_schedule_uses_the_display_rate(self):
        main = MAIN_CPP.read_text()
        self.assertIn("set_refresh_interval(refresh_interval)", main,
                      "the schedule must tick at the panel's refresh rate, not a default")


class TestSnapshotPurity(unittest.TestCase):
    def test_snapshot_never_records_interpolation(self):
        for relative in SNAPSHOT_SOURCES:
            text = (REPO / relative).read_text()
            self.assertNotIn("interpolation", text.lower(),
                             f"{relative} must not carry interpolation state")

    def test_every_continuity_break_drops_the_history(self):
        main = MAIN_CPP.read_text()
        self.assertIn("void reset_interpolation_history()", main)
        load_at = main.index("Java_com_gungnir_recomp_GungnirActivity_nativeLoadSnapshot")
        reset_at = main.index("Java_com_gungnir_recomp_GungnirActivity_nativeResetToTitle")
        next_jni = main.index("extern \"C\" JNIEXPORT", reset_at + 10)
        self.assertIn("reset_interpolation_history", main[load_at:reset_at],
                      "a snapshot load must drop the recorded frames")
        self.assertIn("reset_interpolation_history", main[reset_at:next_jni],
                      "reset to title must drop the recorded frames")
        self.assertIn("reset_interpolation_history", main[:load_at].split("void resume_after_background")[-1],
                      "a background resume must drop the recorded frames")


if __name__ == "__main__":
    unittest.main()

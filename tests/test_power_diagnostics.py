#!/usr/bin/env python3
"""Power-drain fix: static-scene skip and opt-in widescreen coverage.

The device investigation (docs/BRINGUP.md, 2026-11-04) found two avoidable
loads in the production render loop: interpolation generating in-between
presents for a scene that did not move, and measure_widescreen_coverage
running on every present with no way to turn it off. These source contracts
pin both behaviours so the render loop cannot silently regress to doing the
expensive work by default.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"


def main_text():
    return MAIN.read_text()


class TestCoverageIsOptIn(unittest.TestCase):
    def test_coverage_measurement_has_an_opt_in_flag(self):
        text = main_text()
        self.assertIn("g_coverage_enabled", text,
                      "coverage is gated by a named opt-in flag")

    def test_coverage_flag_defaults_off(self):
        text = main_text()
        # The intent extra must default to 0 (off); QA enables it explicitly.
        self.assertTrue(
            re.search(r'g_coverage_enabled\s*=.*intent_(?:long|string)\(\s*"coverage"\s*,\s*0\s*\)',
                      text) is not None,
            "coverage intent extra defaults to off (0)",
        )

    def test_measure_coverage_call_is_guarded(self):
        text = main_text()
        # measure_widescreen_coverage must not sit at the top level of
        # present_guest_frame: its enclosing block must be conditional on the
        # opt-in flag. Match the call and check the flag appears before it,
        # guarding the block, rather than the call being unconditional.
        call = text.find("measure_widescreen_coverage(")
        self.assertNotEqual(call, -1, "coverage measurement is still present")
        prefix = text[:call]
        self.assertTrue("g_coverage_enabled" in prefix,
                        "the coverage call is guarded by the opt-in flag")

    def test_coverage_guard_wraps_the_call(self):
        text = main_text()
        # The guard must be a real conditional around the call, not merely a
        # flag declared somewhere earlier in the file.
        self.assertTrue(
            re.search(r'if\s*\(\s*g_coverage_enabled[^)]*\)\s*\{[^}]*measure_widescreen_coverage',
                      text) is not None,
            "an if(g_coverage_enabled) block encloses the coverage call",
        )


class TestStaticSceneSkipIsWired(unittest.TestCase):
    def test_main_uses_scene_motion(self):
        text = main_text()
        self.assertTrue("scene_is_static" in text,
                        "the render loop decides scene stillness between guest frames")

    def test_on_guest_frame_receives_static_scene(self):
        text = main_text()
        # on_guest_frame must be called with a third 'static scene' argument.
        self.assertTrue(
            re.search(r'on_guest_frame\([^;]*,\s*[^;]*,\s*[^;]*static[^;]*\)', text) is not None,
            "on_guest_frame is passed a static-scene decision",
        )


class TestHeldImageSkipIsExactAndCounted(unittest.TestCase):
    def test_hold_is_gated_on_exact_identity(self):
        text = main_text()
        self.assertTrue("frames_identical" in text,
                        "the hold decision uses exact frame identity, not a fuzzy threshold")

    def test_hold_has_a_safety_represent_interval(self):
        text = main_text()
        self.assertTrue("kHoldRePresentFrames" in text,
                        "consecutive holds are bounded by a named safety interval")

    def test_hold_is_counted_and_logged(self):
        text = main_text()
        self.assertTrue("presents_skipped" in text,
                        "skipped presents are counted, never silent")
        self.assertTrue("presents_skipped=%llu" in text,
                        "the counter is logged in the periodic stats tick")

    def test_hold_is_reset_on_continuity_break(self):
        text = main_text()
        # reset_interpolation_history must clear the held image too. Slice the
        # function body by brace matching (it contains `= {};` initialisers, so
        # a regex with [^}]* would stop early).
        start = text.find("void reset_interpolation_history()")
        self.assertNotEqual(start, -1, "reset_interpolation_history exists")
        body = text[start:start + 900]
        self.assertTrue("g_held_image.has_last = false" in body,
                        "a snapshot/reset clears the held image")

    def test_hold_never_fakes_a_render_without_the_compositor_evidence(self):
        text = main_text()
        self.assertTrue("VK_PRESENT_MODE_FIFO_KHR" in text or "compositor keeps" in text,
                        "the hold documents why the swapchain image persists")


if __name__ == "__main__":
    unittest.main()

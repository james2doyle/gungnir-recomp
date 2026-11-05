#!/usr/bin/env python3
"""9826wejc — widescreen coverage diagnostic wiring contracts.

The widescreen gap must be measured, not described: a bounds-only coverage
computation at present time, a periodic log line with the coverage and
backdrop-only column counts, a one-shot line on a scene change, and no extra
render pass. These are source contracts for the wiring that a C++ unit test
cannot see.

Red-first: written before the diagnostic existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
HOST_CMAKE = REPO / "CMakeLists.txt"
ANDROID_CMAKE = REPO / "cmake" / "AndroidRuntime.cmake"
RENDERER_DOCS = REPO / "docs" / "RENDERER.md"
QA_DOCS = REPO / "docs" / "QA.md"


class TestWidescreenCoverageWiring(unittest.TestCase):
    def test_coverage_is_computed_at_present_time(self):
        main = MAIN_CPP.read_text()
        self.assertIn("measure_widescreen_coverage", main,
                      "the measure must run at present time, not in a render pass")
        self.assertIn("widescreen_coverage.hpp", main)
        self.assertIn("g_widescreen_scale", main,
                      "the scale must be the same one presentation uses")

    def test_periodic_line_reports_the_measured_gap(self):
        main = MAIN_CPP.read_text()
        self.assertIn("widescreen coverage=", main)
        self.assertIn("backdrop_only_columns=", main)
        self.assertIn("hook=", main,
                      "the line must name whether a camera hook is active")
        self.assertIn("percent_sum / samples", main,
                      "the periodic value is averaged over the tick, not one frame")
        self.assertIn("samples=%llu scale=", main,
                      "the periodic line must carry the sample count and scale")
        self.assertIn("widescreen coverage=%llu%%", main,
                      "the periodic line is distinct from the one-shot line")
        self.assertIn("g_coverage.samples = 0u", main,
                      "the tick accumulator must be reset after reporting")

    def test_scene_change_gets_a_one_shot_line(self):
        main = MAIN_CPP.read_text()
        self.assertIn("last_logged_percent", main,
                      "a meaningful step must be deduplicated, not logged per present")
        self.assertIn(">= 10", main,
                      "the step threshold must be explicit, not implicit")

    def test_source_is_built_everywhere(self):
        self.assertIn("runtime/ge/widescreen_coverage.cpp", HOST_CMAKE.read_text())
        self.assertIn("runtime/ge/widescreen_coverage.cpp", ANDROID_CMAKE.read_text())

    def test_docs_state_the_number_is_the_measured_gap(self):
        self.assertIn("measured gap", RENDERER_DOCS.read_text(),
                      "RENDERER.md must say the number is the measured gap")
        self.assertIn("widescreen coverage=", QA_DOCS.read_text(),
                      "QA.md must record the number before/after a hook lands")


if __name__ == "__main__":
    unittest.main()

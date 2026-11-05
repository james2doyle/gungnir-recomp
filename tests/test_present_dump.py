#!/usr/bin/env python3
"""Interpolated-present dump wiring contracts.

The folding artifact appears in neither endpoint guest frame of a real pan
dump, so it can only be located by capturing the frames the presenter actually
shows (the interpolated in-betweens). This dump must be bounded, opt-in via an
intent extra, share the guest-frame dump format, and never change what is
presented.

Red-first: written before the presented-frame dump existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
ANDROID_DOCS = REPO / "docs" / "ANDROID.md"


class TestPresentDumpWiring(unittest.TestCase):
    def test_presented_frames_have_their_own_bounded_opt_in_dump(self):
        main = MAIN_CPP.read_text()
        self.assertIn("dump_presents", main,
                      "an intent extra must name the presented-frame dump file")
        self.assertIn("dump_present_frame", main,
                      "the interpolated frame must actually be written")
        self.assertIn("g_present_dump", main,
                      "the presented dump needs its own bounded state")

    def test_it_uses_the_same_writer_as_the_guest_dump(self):
        main = MAIN_CPP.read_text()
        self.assertIn("write_dump_frame", main,
                      "guest and presented frames must share one format writer")
        self.assertIn("write_dump_frame(g_draw_dump", main)
        self.assertIn("write_dump_frame(g_present_dump", main)

    def test_it_is_called_on_the_presented_vector(self):
        main = MAIN_CPP.read_text()
        self.assertIn("dump_present_frame(*to_present)", main,
                      "the dump must capture to_present, not the guest frame")

    def test_it_is_bounded(self):
        main = MAIN_CPP.read_text()
        self.assertIn("g_present_dump.limit", main)
        self.assertIn("frames_written >= g_present_dump.limit", main,
                      "the presented dump must stop at its frame bound")

    def test_docs_name_the_extra(self):
        self.assertIn("dump_presents", ANDROID_DOCS.read_text(),
                      "the QA intent extra must be documented")


if __name__ == "__main__":
    unittest.main()

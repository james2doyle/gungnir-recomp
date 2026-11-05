#!/usr/bin/env python3
"""c64gg6n7 — camera path trace diagnostic contracts.

The projection matrix reaches the GE as display-list commands 0x3E (projection
matrix index) and 0x3F (projection matrix upload). A bounded host run must be
able to emit those commands with the list PC, the command's own PC and the raw
word, gated by an env var so a normal run is unchanged. These are source
contracts for instrumentation that lives in the HLE media path.

Red-first: written before the diagnostic existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HLE_MEDIA = REPO / "runtime" / "hle" / "hle_media.cpp"


class TestCameraTraceDiagnostic(unittest.TestCase):
    def test_projection_commands_are_logged_with_the_list_and_command_pc(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("GUNGNIR_TRACE_CAMERA", src,
                      "the diagnostic must be opt-in via a named env gate")
        self.assertIn("camera_projection", src,
                      "projection commands must be logged under a named tag")
        self.assertIn("command.pc", src,
                      "the command's own display-list PC must be logged")
        self.assertIn("list.pc", src,
                      "the enqueued list PC must be logged")
        self.assertIn("command.word", src,
                      "the raw command word must be logged for the f24 value")

    def test_gate_defaults_off_and_is_bounded(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("GUNGNIR_TRACE_CAMERA_LIMIT", src,
                      "the diagnostic must be bounded, never an unbounded log")
        self.assertIn("std::getenv(\"GUNGNIR_TRACE_CAMERA\")", src,
                      "an absent env var must leave the run unchanged")

    def test_it_covers_both_projection_opcodes(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("0x3Eu", src, "opcode 0x3E (projection index) must be traced")
        self.assertIn("0x3Fu", src, "opcode 0x3F (projection upload) must be traced")

    def test_it_covers_the_per_draw_transform_stream(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("camera_transform", src,
                      "world/view/bone matrices must be logged under a distinct tag")
        for opcode in ("0x3Au", "0x3Bu", "0x3Cu", "0x3Du", "0x2Au", "0x2Bu"):
            self.assertIn(opcode, src,
                          f"GE matrix opcode {opcode} must be traced for per-draw transforms")


if __name__ == "__main__":
    unittest.main()

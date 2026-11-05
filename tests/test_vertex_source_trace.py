#!/usr/bin/env python3
"""Contracts for the GE vertex-source diagnostic.

A stable per-draw identity must come from the guest's CPU-side object data,
not from GE transform matrices (which the transform trace showed are constant).
The first step is to see the vertex source address each primitive draw reads.
Opt-in and bounded, like the other trace diagnostics.

Red-first: written before the diagnostic existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HLE_MEDIA = REPO / "runtime" / "hle" / "hle_media.cpp"


class TestVertexSourceDiagnostic(unittest.TestCase):
    def test_it_logs_the_vertex_address_of_each_draw(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("GUNGNIR_TRACE_VERTEX_SOURCE", src,
                      "the diagnostic must be opt-in via a named env gate")
        self.assertIn("vertex_source", src,
                      "vertex source addresses must be logged under a named tag")
        self.assertIn("command.address", src,
                      "the resolved vertex address must be logged")
        self.assertIn("command.argument", src,
                      "the raw address argument must be logged too")

    def test_it_is_bounded_and_defaults_off(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("GUNGNIR_TRACE_VERTEX_SOURCE_LIMIT", src)
        self.assertIn('std::getenv("GUNGNIR_TRACE_VERTEX_SOURCE")', src)

    def test_it_covers_both_address_commands(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("CommandKind::VertexAddress", src,
                      "GE vertex address command 0x01")
        self.assertIn("CommandKind::IndexAddress", src,
                      "GE index address command 0x02")

    def test_it_covers_prim_commands_with_their_count(self):
        src = HLE_MEDIA.read_text()
        self.assertIn("CommandKind::Primitive", src,
                      "PRIM commands must be traced to see per-draw slots")
        self.assertIn("count=", src,
                      "the vertex count must be logged for each PRIM")
        self.assertIn("command.count", src)


if __name__ == "__main__":
    unittest.main()

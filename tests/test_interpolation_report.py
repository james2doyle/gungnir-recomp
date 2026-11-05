#!/usr/bin/env python3
"""Contracts for tools/interpolation_report.py — the guest-frame dump reader.

The dump is the only way to answer presentation questions with the frames the
game actually produced, so the reader must decode the documented format exactly
and must flag the two failure modes that were invisible from a recording:
a pair whose vertices cannot be moved independently (shape collapse) and a
bucket whose arrival order changes between frames.
"""
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import interpolation_report as report  # noqa: E402


def draw(key, points, through=False):
    return {"key": key, "through": through, "vertices": points}


def encode(frames):
    chunks = []
    for frame in frames:
        chunks.append(struct.pack("<I", len(frame)))
        for item in frame:
            chunks.append(struct.pack("<QBI", item["key"], 1 if item["through"] else 0,
                                      len(item["vertices"])))
            for point in item["vertices"]:
                chunks.append(struct.pack("<5f", *point))
    return b"".join(chunks)


def quad(x, y, size=10.0, key=0x1):
    return draw(key, [(x, y, 5.0, 0.0, 0.0), (x + size, y, 5.0, 1.0, 0.0),
                      (x, y + size, 5.0, 0.0, 1.0), (x + size, y + size, 5.0, 1.0, 1.0)])


def triangle(x, y, key=0x2):
    return draw(key, [(x, y, 5.0, 0.0, 0.0), (x + 10.0, y, 5.0, 1.0, 0.0),
                      (x, y + 10.0, 5.0, 0.0, 1.0)])


class TestDumpReader(unittest.TestCase):
    def test_round_trips_the_documented_format(self):
        frames = [[quad(0.0, 0.0)], [quad(2.0, 0.0), triangle(40.0, 40.0)]]
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as handle:
            handle.write(encode(frames))
            path = handle.name
        loaded = report.load(path)
        self.assertEqual([len(f) for f in loaded], [1, 2])
        self.assertEqual(loaded[1][1]["key"], 0x2)
        self.assertAlmostEqual(loaded[1][0]["vertices"][0][0], 2.0)
        Path(path).unlink()

    def test_reports_a_static_scene_as_still(self):
        frames = [[quad(0.0, 0.0)] for _ in range(3)]
        stats = report.analyse_pair(frames[0], frames[1])
        self.assertEqual(stats["pairs"], 1)
        self.assertLess(stats["frame_motion"], 0.01)
        self.assertEqual(stats["collapses_vertex"], 0)
        self.assertEqual(stats["order_breaks"], 0)

    def test_flags_a_collapsing_pair(self):
        # The same triangle rotated 180 degrees about its centre: blending the
        # corners independently collapses it to a point.
        before = [triangle(0.0, 0.0)]
        after = [draw(0x2, [(26.6667, 6.6667, 5.0, 0.0, 0.0),
                            (16.6667, 6.6667, 5.0, 1.0, 0.0),
                            (26.6667, -3.3333, 5.0, 0.0, 1.0)])]
        guarded = report.analyse_pair(before, after)
        self.assertEqual(guarded["collapses_vertex"], 0,
                         "the runtime translates it, so nothing collapses")
        self.assertEqual(guarded["rigid_pairs"], 1,
                         "and the reader must say the shape-change answer applied")
        unguarded = report.analyse_pair(before, after, rigid_rule=False)
        self.assertEqual(unguarded["collapses_vertex"], 1,
                         "with the answer disabled the collapse is what the user sees")

    def test_flags_an_order_break(self):
        # Four items of one bucket; the last one wraps to the far end.
        before = [quad(float(i) * 10.0, 0.0) for i in range(4)]
        after = [quad(float(i) * 10.0 + 3.0, 0.0) for i in range(3)]
        after.append(quad(200.0, 0.0))
        stats = report.analyse_pair(before, after)
        self.assertGreaterEqual(stats["order_breaks"], 1,
                                "a bucket whose order changed must be reported")

    def test_verbose_names_the_bucket_that_broke(self):
        before = [quad(float(i) * 10.0, 0.0) for i in range(4)]
        after = [quad(float(i) * 10.0 + 3.0, 0.0) for i in range(3)] + [quad(200.0, 0.0)]
        out = io.StringIO()
        report.report([before, after], out=out, verbose=True)
        self.assertIn("order break key=0x1", out.getvalue())
        self.assertIn("bucket=4", out.getvalue())

    def test_full_report_names_the_buckets_and_the_pairs(self):
        frames = [[quad(0.0, 0.0), quad(100.0, 0.0)], [quad(2.0, 0.0), quad(102.0, 0.0)]]
        out = io.StringIO()
        report.report(frames, out=out)
        text = out.getvalue()
        self.assertIn("frames=2", text)
        self.assertIn("identity buckets=1", text)
        self.assertIn("0->1:", text)
        self.assertIn("collapses vertex=0", text)


if __name__ == "__main__":
    unittest.main()

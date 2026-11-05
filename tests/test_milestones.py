#!/usr/bin/env python3
"""T13h — milestone ledger contract (red-first).

Every entry in data/milestones.json carries criterion/measure/evidence/state;
the tools/milestones.py ratchet checker fails a silently lowered figure;
public content is counts-only (no screenshot paths, no game hashes).
M4 references existing QA reports once recorded; while planned it needs a
criterion, not measures.
"""
import json
import re
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))

import milestones as ledger_check  # noqa: E402

LEDGER = REPO / "data" / "milestones.json"
QA_DIR = REPO / "docs" / "qa"

BANNED = re.compile(r"\.(png|jpg|jpeg|bmp|tga|dds|mp4|chd|iso|cso)\b", re.I)
HEX64 = re.compile(r"\b[0-9a-f]{64}\b", re.I)


def load():
    return json.loads(LEDGER.read_text(encoding="utf-8"))


class TestMilestoneLedger(unittest.TestCase):
    def setUp(self):
        if not LEDGER.is_file():
            self.fail("data/milestones.json is missing")
        self.ledger = load()

    def test_every_entry_has_criterion_measure_evidence_state(self):
        entries = self.ledger["milestones"]
        self.assertIn("M4", entries, "ledger must track M4")
        for mid, entry in entries.items():
            self.assertIn("state", entry, f"{mid} has no state")
            self.assertIn(entry["state"], ("planned", "open", "closed"),
                          f"{mid} has unknown state {entry['state']!r}")
            criteria = entry.get("criteria") or entry.get("criterion")
            self.assertTrue(criteria, f"{mid} has no criterion/criteria")
            if entry["state"] == "closed":
                self.assertTrue(entry.get("evidence"), f"closed {mid} has no evidence")
                self.assertTrue(
                    entry.get("recorded") or entry.get("measure") or entry.get("measures"),
                    f"closed {mid} has no recorded date or measure",
                )

    def test_m4_references_qa_reports_once_recorded(self):
        m4 = self.ledger["milestones"]["M4"]
        if m4.get("state") == "planned":
            self.assertTrue(m4.get("criteria") or m4.get("criterion"),
                            "planned M4 must still state its criterion")
            return
        blob = json.dumps(m4)
        self.assertIn("docs/qa/", blob, "recorded M4 must point at dated QA reports")
        for report in re.findall(r"docs/qa/[\w\-.]+\.md", blob):
            self.assertTrue((QA_DIR / Path(report).name).is_file(),
                            f"M4 evidence {report} has no file in docs/qa/")

    def test_ratchet_passes_identical_ledger(self):
        self.assertEqual(
            ledger_check.check_ratchet(self.ledger, self.ledger), [],
            "an unchanged ledger must pass the ratchet",
        )

    def test_ratchet_fails_a_silently_lowered_figure(self):
        import copy
        doctored = copy.deepcopy(self.ledger)
        lowered = ledger_check.lower_one_figure(doctored)
        self.assertTrue(lowered, "no closed milestone carries a figure to lower")
        drops = ledger_check.check_ratchet(self.ledger, doctored)
        self.assertTrue(drops, "a silently lowered figure must fail the ratchet")
        self.assertTrue(
            all("lowered" in drop for drop in drops),
            f"ratchet drops must name the lowered figure: {drops}",
        )

    def test_public_content_is_counts_only(self):
        blob = LEDGER.read_text(encoding="utf-8")
        self.assertIsNone(BANNED.search(blob),
                          "ledger must not reference screenshots, video or disc images")
        self.assertIsNone(HEX64.search(blob),
                          "ledger must not carry dump hashes (counts and criteria only)")


if __name__ == "__main__":
    unittest.main()

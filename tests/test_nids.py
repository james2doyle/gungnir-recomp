#!/usr/bin/env python3
"""T6a — NID registry coverage and provenance.

The runtime binds HLE handlers by (library, NID). Gungnir imports 215 symbols
across 23 libraries; every one must resolve to a registry name before T6 can
claim a loud, named unknown-NID path.

Red-first: this suite was written before configs/nids.csv existed.
"""
import csv
import hashlib
import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CSV = REPO / "configs" / "nids.csv"
PIN = REPO / "configs" / "nids.csv.sha256"
PROVENANCE = REPO / "third_party" / "pspsdk-nids" / "PROVENANCE.md"
LICENSE = REPO / "third_party" / "pspsdk-nids" / "LICENSE"
MANIFEST = REPO / "build" / "game" / "manifest.json"
PSPSDK_PIN = "c3da4314d783ac26323e5cdaf8163ac369c2089d"


def load_registry(path=CSV):
    rows = {}
    with open(path, newline="") as handle:
        lines = [line for line in handle if not line.startswith("#")]
    for row in csv.reader(lines):
        if len(row) != 3:
            raise AssertionError(f"unexpected CSV row: {row}")
        library, nid_text, name = row
        key = (library, int(nid_text, 0))
        if key in rows:
            raise AssertionError(f"duplicate NID {library}:{nid_text}")
        if not name:
            raise AssertionError(f"empty name for {key}")
        rows[key] = name
    return rows


class TestRegistryShape(unittest.TestCase):
    def test_registry_exists_and_is_well_formed(self):
        self.assertTrue(CSV.is_file(), "configs/nids.csv missing")
        rows = load_registry()
        self.assertGreater(len(rows), 500)
        self.assertIn(("ThreadManForUser", 0x446D8DE6), rows)

    def test_registry_digest_is_pinned(self):
        self.assertTrue(PIN.is_file(), "configs/nids.csv.sha256 missing")
        expected = PIN.read_text().split()[0].strip()
        actual = hashlib.sha256(CSV.read_bytes()).hexdigest()
        self.assertEqual(actual, expected)

    def test_provenance_and_license_are_recorded(self):
        self.assertTrue(PROVENANCE.is_file())
        self.assertTrue(LICENSE.is_file())
        text = PROVENANCE.read_text()
        self.assertIn(PSPSDK_PIN, text)
        self.assertIn("BSD-3-Clause", text)
        self.assertIn("configs/nids.csv", text)
        self.assertIn("Redistribution and use in source and binary forms", LICENSE.read_text())


@unittest.skipUnless(MANIFEST.exists(), "needs private game input (game.iso)")
class TestGungnirCoverage(unittest.TestCase):
    def test_every_import_resolves(self):
        rows = load_registry()
        manifest = json.loads(MANIFEST.read_text())
        module = next(m for m in manifest["modules"]
                      if m.get("module_name") == "9th")
        missing = []
        for entry in module["imports"]:
            key = (entry["library"], int(entry["nid"], 0))
            if key not in rows:
                missing.append(f"{entry['library']}:{entry['nid']}")
        self.assertEqual(missing, [],
                         f"{len(missing)} Gungnir imports have no registry name")


if __name__ == "__main__":
    unittest.main()

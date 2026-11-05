#!/usr/bin/env python3
"""T13h — release checklist contract (red-first).

docs/RELEASE.md must reference the scripts and gates that actually exist,
state every required artifact rule, and contain no machine-local absolute
paths. docs/PROGRESS.md carries milestone state, counts, criteria and named
gaps only — never game content or dump identities.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
RELEASE = REPO / "docs" / "RELEASE.md"
PROGRESS = REPO / "docs" / "PROGRESS.md"

ABSOLUTE = re.compile(r"(?m)^(?:[^`\n]*\s)?(?:/home/|/Users/|C:\\|[A-Z]:\\)\S+")
HEX64 = re.compile(r"\b[0-9a-f]{64}\b", re.I)
BANNED = re.compile(r"\.(png|jpg|jpeg|bmp|tga|dds|mp4|chd|iso|cso)\b", re.I)


class TestReleaseChecklist(unittest.TestCase):
    def setUp(self):
        if not RELEASE.is_file():
            self.fail("docs/RELEASE.md is missing")
        self.text = RELEASE.read_text(encoding="utf-8")

    def test_checklist_references_existing_scripts_and_gates(self):
        for token in (
            "scripts/android/build.sh",
            "repository_check",
            "clean clone",
            "local.properties",
        ):
            self.assertIn(token.lower(), self.text.lower(),
                          f"RELEASE.md does not cover {token!r}")
        for script in sorted(set(re.findall(r"scripts/[A-Za-z0-9_./-]+\.sh", self.text))):
            self.assertTrue((REPO / script).is_file(),
                            f"RELEASE.md references missing script {script}")

    def test_every_required_artifact_rule_is_present(self):
        for rule in (
            "no generated",
            "signing",
            "version",
            "notices",
            "REUSE",
            "decrypt",
        ):
            self.assertIn(rule.lower(), self.text.lower(),
                          f"RELEASE.md is missing the {rule!r} artifact rule")

    def test_no_absolute_paths(self):
        hits = [line for line in self.text.splitlines() if ABSOLUTE.search(line)]
        self.assertEqual(hits, [], f"RELEASE.md carries machine-local paths: {hits[:3]}")


class TestPublicStatus(unittest.TestCase):
    def setUp(self):
        if not PROGRESS.is_file():
            self.fail("docs/PROGRESS.md is missing")
        self.text = PROGRESS.read_text(encoding="utf-8")

    def test_progress_names_milestones_and_gaps(self):
        lowered = self.text.lower()
        for token in ("m4", "milestone", "gap"):
            self.assertIn(token, lowered,
                          f"PROGRESS.md does not mention {token!r}")

    def test_progress_is_counts_only(self):
        self.assertIsNone(BANNED.search(self.text),
                          "PROGRESS.md must not reference screenshots, video or disc images")
        self.assertIsNone(HEX64.search(self.text),
                          "PROGRESS.md must not carry dump hashes")


if __name__ == "__main__":
    unittest.main()

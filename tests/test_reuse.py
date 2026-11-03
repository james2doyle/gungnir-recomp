#!/usr/bin/env python3
"""T13e — REUSE coverage contract (red-first).

Every tracked file must be covered by a REUSE.toml annotation whose
license is in the project-approved set and whose third-party zones match
dependencies.json. Mirrors what `reuse lint` enforces in CI; this test is
the locally runnable half (the reuse binary is not vendored).
"""
import re
import tomllib
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REUSE_TOML = REPO / "REUSE.toml"

APPROVED = {
    "MIT", "ISC", "Zlib", "Unlicense", "CC0-1.0",
    "BSD-1-Clause", "BSD-2-Clause", "BSD-3-Clause",
    "Apache-2.0", "Apache-1.1",
    "LGPL-2.0", "LGPL-2.1", "LGPL-3.0", "GPL-2.0", "GPL-2.0-or-later",
    "GPL-3.0", "GPL-3.0-or-later", "GPL-1.0",
    "LGPL-2.0-or-later", "LGPL-2.1-or-later", "LGPL-3.0-or-later",
}


def tracked_files():
    import subprocess
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, cwd=REPO)
    return [line for line in out.stdout.splitlines() if line]


def annotation_covers(pattern, path):
    if pattern == "**":
        return True
    if pattern.endswith("/**"):
        return path == pattern[:-3] or path.startswith(pattern[:-3] + "/")
    if "*" in pattern:
        return re.fullmatch(re.escape(pattern).replace(r"\*", ".*"), path) is not None
    return path == pattern


class TestReuse(unittest.TestCase):
    def setUp(self):
        check(REUSE_TOML.is_file(), "REUSE.toml missing")
        with open(REUSE_TOML, "rb") as handle:
            self.reuse = tomllib.load(handle)
        self.annotations = self.reuse.get("annotations", [])
        check(self.annotations, "REUSE.toml has no annotations")

    def test_every_tracked_file_covered(self):
        uncovered = []
        for path in tracked_files():
            covered = any(
                annotation_covers(pat, path)
                for ann in self.annotations
                for pat in (ann.get("path") if isinstance(ann.get("path"), list) else [ann.get("path")])
                if pat
            )
            if not covered:
                uncovered.append(path)
        check(not uncovered, f"uncovered files: {uncovered[:8]}")

    def test_licenses_approved(self):
        for ann in self.annotations:
            lic = ann.get("SPDX-License-Identifier", "")
            check(lic in APPROVED, f"unapproved license {lic!r} in REUSE.toml")

    def test_license_texts_present(self):
        licenses_dir = REPO / "LICENSES"
        used = {ann.get("SPDX-License-Identifier") for ann in self.annotations}
        for lic in used:
            check((licenses_dir / f"{lic}.txt").is_file(), f"LICENSES/{lic}.txt missing")

    def test_third_party_zones_match_dependencies(self):
        import json
        deps = json.loads((REPO / "dependencies.json").read_text())["dependencies"]
        zones = sorted(d["path"] for d in deps if str(d.get("path", "")).startswith("third_party/"))
        for zone in zones:
            hit = any(
                pat in (zone, zone + "/**")
                for ann in self.annotations
                for pat in (ann.get("path") if isinstance(ann.get("path"), list) else [ann.get("path")])
                if pat
            )
            check(hit, f"dependencies zone {zone} has no REUSE annotation")


def check(cond, reason):
    if not cond:
        raise AssertionError(reason)


if __name__ == "__main__":
    unittest.main()

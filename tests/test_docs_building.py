#!/usr/bin/env python3
"""T13c — from-scratch build documentation contract.

Red-first: neither `docs/BUILDING.md` nor this contract existed when the test
was written. The test also rejects stale script flags and dead relative links,
so the documented pipeline cannot drift away from the scripts.
"""
import re
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILDING = REPO / "docs" / "BUILDING.md"
ANDROID = REPO / "docs" / "ANDROID.md"
README = REPO / "README.md"

FENCED = re.compile(r"```[a-zA-Z0-9_-]*\n(.*?)```", re.S)
SCRIPT = re.compile(r"(?<![\w./-])((?:scripts|tools)/[A-Za-z0-9_./-]+\.(?:sh|py))")
PATH_TOKEN = re.compile(r"`([A-Za-z0-9_./-]+\.(?:py|sh|toml|csv|json|cmake|md))`")
LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
FLAG = re.compile(r"(?<![\w-])(--[a-z][a-z-]*)(?=[\s=,)]|$)")
GRADLE_PROP = re.compile(r"(-Pgungnir\.[A-Za-z]+)")


def read(path):
    return path.read_text(encoding="utf-8")


def fenced_blocks(text):
    return [block for block in FENCED.findall(text)]


def help_text(script):
    proc = subprocess.run([str(script), "--help"], capture_output=True, text=True, timeout=60)
    return proc.stdout + proc.stderr


class TestBuildDocumentation(unittest.TestCase):
    def setUp(self):
        if not BUILDING.is_file():
            self.fail("docs/BUILDING.md is missing")
        self.text = read(BUILDING)

    def test_building_covers_the_pipeline(self):
        for phrase in (
            "intake",
            "decrypt",
            "analyze",
            "codegen",
            "cmake",
            "scripts/android/build.sh",
            "scripts/android/install.sh",
            "scripts/android/run.sh",
            "scripts/android/screenshot.sh",
            "--offline",
        ):
            self.assertIn(phrase, self.text, f"BUILDING.md does not document {phrase!r}")
        for marker in ("named failure", "gitignored", "ISO"):
            self.assertIn(marker, self.text.lower() if marker.islower() else self.text)

    def test_scripts_referenced_by_the_doc_exist(self):
        for relative in sorted(set(SCRIPT.findall(self.text))):
            path = REPO / relative
            self.assertTrue(path.is_file(), f"BUILDING.md names a missing script: {relative}")

    def test_documented_build_flags_exist(self):
        blocks = [block for block in fenced_blocks(self.text) if "scripts/android/build.sh" in block]
        self.assertTrue(blocks, "BUILDING.md has no build.sh command block")
        help_output = help_text(REPO / "scripts" / "android" / "build.sh")
        for block in blocks:
            for flag in set(FLAG.findall(block)) | set(GRADLE_PROP.findall(block)):
                self.assertIn(flag, help_output, f"documented flag {flag} is not in build.sh --help")

    def test_documented_paths_exist(self):
        for relative in sorted(set(PATH_TOKEN.findall(self.text))):
            if relative.startswith(("build/", "profiles/", "game.", "imported.")):
                continue
            self.assertTrue((REPO / relative).exists(), f"BUILDING.md names a missing path: {relative}")

    def test_no_machine_specific_paths(self):
        for pattern in (r"/home/", r"/Users/", r"[A-Za-z]:\\\\", r"/opt/android-sdk"):
            self.assertIsNone(re.search(pattern, self.text), f"machine-specific path pattern {pattern} in BUILDING.md")

    def test_no_dump_hashes_or_screenshots(self):
        self.assertIsNone(re.search(r"\b[0-9a-fA-F]{64}\b", self.text), "BUILDING.md contains a SHA-256")
        self.assertNotIn(".png", self.text, "BUILDING.md should not embed screenshots")
        self.assertIsNone(re.search(r"(?i)full speed|60 ?fps", self.text),
                          "BUILDING.md must not claim performance the tree lacks")

    def test_relative_links_resolve(self):
        for target in set(LINK.findall(self.text)):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            path = (BUILDING.parent / target.split("#", 1)[0]).resolve()
            self.assertTrue(path.exists(), f"BUILDING.md has a dead link: {target}")

    def test_android_doc_reflects_the_t13_runtime(self):
        android = read(ANDROID)
        for phrase in (
            "audio_mixer",
            "dispatch_limit",
            "wall_seconds",
            "audio_smoke",
            "debug_quad",
            "files/psp",
            "docs/qa/2026-11-01-t13b-title.md",
            "MPEG",
        ):
            self.assertIn(phrase, android, f"ANDROID.md is missing {phrase!r}")
        self.assertNotIn("title-screen rendering path and menu\nbring-up are T13b", android)
        self.assertNotIn("Actual game audio is T12", android)

    def test_readme_links_the_building_guide(self):
        readme = read(README)
        self.assertIn("docs/BUILDING.md", readme)
        self.assertIn("](docs/BUILDING.md)", readme, "README.md should link the building guide")


if __name__ == "__main__":
    unittest.main()

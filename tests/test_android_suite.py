#!/usr/bin/env python3
"""T9f — consolidated Android gate (TC-9.1–TC-9.4) and doc/script checks."""
import subprocess
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ANDROID_DOC = REPO / "docs" / "ANDROID.md"
QA_REPORT = REPO / "docs" / "qa" / "2026-10-30-t9-android-shell.md"
SCRIPTS = REPO / "scripts" / "android"

MODULES = (
    "tests.test_android_build",
    "tests.test_android_app",
    "tests.test_game_data",
    "tests.test_affinity",
)


class TestAndroidSuite(unittest.TestCase):
    def test_host_modules(self):
        proc = subprocess.run([sys.executable, "-m", "unittest", *MODULES],
                              cwd=REPO, capture_output=True, text=True, timeout=900)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"Android host gates failed:\n{output}")

    def test_docs_cover_the_shell(self):
        self.assertTrue(ANDROID_DOC.is_file(), "docs/ANDROID.md exists")
        text = ANDROID_DOC.read_text()
        for heading in ("## Build", "## Deploy", "## Game data",
                        "## Lifecycle and orientation", "## Named failures"):
            self.assertIn(heading, text, f"missing section: {heading}")
        self.assertIn("262766592", text)
        self.assertIn("183F3EDC", text)
        self.assertIn("T13", text)

    def test_scripts_and_qa_report(self):
        for name in ("build.sh", "install.sh", "run.sh", "logcat.sh",
                     "screenshot.sh", "lifecycle.sh"):
            path = SCRIPTS / name
            self.assertTrue(path.is_file(), f"{name} exists")
            self.assertTrue(path.stat().st_mode & 0o111, f"{name} is executable")
        self.assertTrue(QA_REPORT.is_file(), "T9 QA report exists")
        report = QA_REPORT.read_text()
        self.assertIn("PASS", report)
        self.assertIn("TC-9.4", report)

    def test_screenshot_output_is_gitignored(self):
        proc = subprocess.run(["git", "check-ignore", "-q", "build/qa/android/probe.png"],
                              cwd=REPO)
        self.assertEqual(proc.returncode, 0, "build/qa is covered by .gitignore")


if __name__ == "__main__":
    unittest.main()

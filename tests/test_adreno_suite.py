#!/usr/bin/env python3
"""T10f — consolidated Adreno driver gate (TC-10.1–TC-10.4).

Runs the host package/loader/inventory/repository gates and checks the
finished driver contract: package source and risks, supported schemas, UI and
private-storage rules, fallback diagnostics, provenance, named gaps, mutation
evidence and the physical-device QA report.

No driver archive is downloaded or committed by this test.
"""
import json
import subprocess
import sys
import unittest
from pathlib import Path

from tests import suite_support

REPO = Path(__file__).resolve().parent.parent
DRIVERS_DOC = REPO / "docs" / "ANDROID_DRIVERS.md"
ANDROID_DOC = REPO / "docs" / "ANDROID.md"
README = REPO / "README.md"
QA_REPORT = REPO / "docs" / "qa" / "2026-10-31-t10e-driver-matrix.md"
DEPS = REPO / "dependencies.json"

MODULES = (
    "tests.test_adreno_docs",
    "tests.test_driver_package",
    "tests.test_vulkan_loader",
    "tests.test_driver_store",
    "tests.test_repository_check",
)

NAMED_ERRORS = (
    "driver_zip_missing",
    "driver_zip_unsupported",
    "driver_zip_slip",
    "driver_entry_duplicate",
    "driver_entry_oversize",
    "driver_entry_name",
    "driver_entry_encrypted",
    "driver_meta_invalid",
    "driver_meta_schema",
    "driver_meta_name",
    "driver_library_missing",
    "driver_library_ambiguous",
    "driver_elf_class",
    "driver_elf_machine",
    "driver_elf_type",
    "driver_elf_truncated",
    "driver_elf_dynsym",
    "driver_abi_unsupported",
    "driver_storage_unavailable",
    "driver_hook_library_missing",
    "driver_id_invalid",
    "vulkan_loader_unavailable",
    "driver_load_failed",
)


class TestAdrenoSuite(unittest.TestCase):
    def test_host_modules(self):
        missing = suite_support.missing(MODULES)
        if not missing:
            self.skipTest("child modules are discovered separately")
        proc = subprocess.run(
            [sys.executable, "-m", "unittest", *missing],
            cwd=REPO, capture_output=True, text=True, timeout=1200)
        output = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, f"Adreno host gates failed:\n{output}")

    def test_docs_cover_the_contract(self):
        text = DRIVERS_DOC.read_text()
        for heading in (
            "## Where driver packages come from",
            "## Decision",
            "## Package contract",
            "## Install and selection flow",
            "## Hard constraints",
            "## Fail-closed errors",
            "## Troubleshooting",
            "## No-bundling policy",
            "## Provenance",
            "## Risks",
            "## Known gaps",
            "## Evidence status",
            "## Mutation evidence",
            "## References",
        ):
            self.assertIn(heading, text, f"missing section: {heading}")
        for needle in (
            "release channel",
            "does not host, mirror, sign",
            "no warranty",
            "meta.json",
            "schemaVersion",
            "libvulkan.so",
            "ACTION_OPEN_DOCUMENT",
            "selection.txt",
            "getFilesDir()",
            "shared storage",
            "driver_fallback",
            "never committed",
        ):
            self.assertIn(needle, text, f"driver doc records: {needle}")
        for error in NAMED_ERRORS:
            self.assertIn(error, text, f"named error documented: {error}")

    def test_provenance_pins_match_dependencies(self):
        entries = {entry["name"]: entry for entry in
                   json.loads(DEPS.read_text())["dependencies"]}
        text = DRIVERS_DOC.read_text()
        for name in ("libadrenotools", "liblinkernsbypass"):
            pin = entries[name]
            self.assertEqual(pin["license"], "BSD-2-Clause")
            self.assertIn(pin["commit"], text, f"{name} commit recorded")
            self.assertIn(pin["archive_sha256"], text, f"{name} archive digest recorded")
        for name in ("miniz", "nlohmann-json"):
            pin = entries[name]
            self.assertEqual(pin["license"], "MIT")
            self.assertIn(pin["commit"], text, f"{name} commit recorded")
            self.assertIn(pin["version"], text, f"{name} version recorded")
        self.assertIn("No local patches", text)
        self.assertIn("third_party/*/PROVENANCE.md", text)

    def test_docs_are_linked(self):
        self.assertIn("ANDROID_DRIVERS.md", ANDROID_DOC.read_text())
        self.assertIn("docs/ANDROID_DRIVERS.md", README.read_text())

    def test_named_gaps_and_evidence_labels(self):
        text = DRIVERS_DOC.read_text()
        for needle in (
            "Non-Adreno",
            "32-bit",
            "API 28",
            "tmpLibDir",
            "auto-update",
            "ADPKG",
            "safe mode",
            "VERIFIED",
            "HYPOTHESIS",
            "UNSUPPORTED",
            "PLANNED",
        ):
            self.assertIn(needle, text, f"gap or evidence label recorded: {needle}")

    def test_mutation_evidence_is_recorded(self):
        text = DRIVERS_DOC.read_text()
        for failure in (
            "system path never calls the custom opener",
            "bad_function_call",
            "failed custom handle is closed",
        ):
            self.assertIn(failure, text, f"mutation evidence recorded: {failure}")

    def test_qa_report_records_all_cases(self):
        self.assertTrue(QA_REPORT.is_file(), "T10e QA report exists")
        report = QA_REPORT.read_text()
        for needle in ("PASS", "TC-10.1", "TC-10.2", "TC-10.3", "TC-10.4",
                       "Rutabaga", "11137ce"):
            self.assertIn(needle, report, f"QA report records: {needle}")

    def test_no_tracked_driver_blobs(self):
        proc = subprocess.run(["git", "ls-files", "-z"], cwd=REPO,
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, "git ls-files works")
        files = [name for name in proc.stdout.split("\0") if name]
        forbidden = []
        for name in files:
            suffix = Path(name).suffix.lower()
            if suffix in {".zip", ".so", ".apk", ".aab", ".jar", ".dll", ".dylib"}:
                forbidden.append(name)
            if name.startswith(("drivers/",
                                "android/app/src/main/assets/drivers/",
                                "android/app/src/main/jniLibs/")):
                forbidden.append(name)
        self.assertEqual(forbidden, [], f"driver blobs are tracked: {forbidden}")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T10a — driver package contract, loader adoption and provenance pin."""
import json
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DRIVERS_DOC = REPO / "docs" / "ANDROID_DRIVERS.md"
PROVENANCE = REPO / "docs" / "PROVENANCE.md"
DEPS = REPO / "dependencies.json"

ADRENOTOOLS_COMMIT = "8fae8ce254dfc1344527e05301e43f37dea2df80"
ADRENOTOOLS_ARCHIVE = "ceffce971676d4cfdf348a082df06fc92a1dca6d95bea892a480d63f200961cb"
LINKERNSBYPASS_COMMIT = "aa3975893d83ef1bc84c321ec60c65fbf1287887"
LINKERNSBYPASS_ARCHIVE = "da1128c8aa771c4d24766b53a47822d0717baa5c536ca8491220402942b80638"

NAMED_ERRORS = (
    "driver_zip_unsupported",
    "driver_zip_slip",
    "driver_meta_invalid",
    "driver_meta_schema",
    "driver_library_missing",
    "driver_library_ambiguous",
    "driver_elf_class",
    "driver_elf_machine",
    "driver_elf_type",
    "driver_elf_truncated",
    "driver_min_api",
    "driver_hook_library_missing",
    "driver_load_failed",
)


class TestDriverContract(unittest.TestCase):
    def test_dependency_pins(self):
        data = json.loads(DEPS.read_text())
        entries = {entry["name"]: entry for entry in data["dependencies"]}

        adrenotools = entries["libadrenotools"]
        self.assertEqual(adrenotools["type"], "fetch")
        self.assertEqual(adrenotools["commit"], ADRENOTOOLS_COMMIT)
        self.assertEqual(adrenotools["license"], "BSD-2-Clause")
        self.assertEqual(adrenotools["archive_sha256"], ADRENOTOOLS_ARCHIVE)
        self.assertIn(ADRENOTOOLS_COMMIT, adrenotools["url"])
        self.assertEqual(adrenotools["path"], "third_party/libadrenotools")

        linker = entries["liblinkernsbypass"]
        self.assertEqual(linker["type"], "fetch")
        self.assertEqual(linker["commit"], LINKERNSBYPASS_COMMIT)
        self.assertEqual(linker["license"], "BSD-2-Clause")
        self.assertEqual(linker["archive_sha256"], LINKERNSBYPASS_ARCHIVE)
        self.assertIn(LINKERNSBYPASS_COMMIT, linker["url"])
        self.assertEqual(linker["path"], "third_party/liblinkernsbypass")

    def test_license_files_and_provenance_exist(self):
        for name in ("libadrenotools", "liblinkernsbypass"):
            base = REPO / "third_party" / name
            self.assertTrue((base / "LICENSE").is_file(), f"{name} license text")
            self.assertTrue((base / "PROVENANCE.md").is_file(), f"{name} provenance")
        ledger = PROVENANCE.read_text()
        self.assertIn("libadrenotools", ledger)
        self.assertIn("liblinkernsbypass", ledger)
        self.assertIn(ADRENOTOOLS_COMMIT[:8], ledger)
        policy = (REPO / "docs" / "SOURCE_PROVENANCE.md").read_text()
        self.assertIn("libadrenotools", policy)
        self.assertIn(ADRENOTOOLS_COMMIT, policy)
        self.assertIn("BSD-2-Clause", policy)

    def test_doc_records_the_decision_and_contract(self):
        self.assertTrue(DRIVERS_DOC.is_file(), "docs/ANDROID_DRIVERS.md exists")
        text = DRIVERS_DOC.read_text()
        for heading in ("## Decision", "## Package contract", "## Install and selection flow",
                        "## Hard constraints", "## Fail-closed errors",
                        "## No-bundling policy", "## Risks", "## Known gaps"):
            self.assertIn(heading, text, f"missing section: {heading}")
        self.assertIn("bylaws/libadrenotools", text)
        self.assertIn("BSD-2-Clause", text)
        self.assertIn(ADRENOTOOLS_COMMIT, text)
        self.assertIn(LINKERNSBYPASS_COMMIT, text)

    def test_package_schema_is_explicit(self):
        text = DRIVERS_DOC.read_text()
        for field in ("schemaVersion", "name", "libraryName", "minApi", "vendor",
                      "driverVersion"):
            self.assertIn(field, text, f"meta.json field {field} documented")
        self.assertIn("libvulkan.so", text)
        self.assertIn("exactly one", text)

    def test_hard_constraints_are_recorded(self):
        text = DRIVERS_DOC.read_text()
        for needle in ("useLegacyPackaging = true", "nativeLibraryDir",
                       "getFilesDir()", "tmpLibDir", "API 28", "arm64-v8a"):
            self.assertIn(needle, text, f"constraint recorded: {needle}")
        self.assertIn("never", text.lower())

    def test_named_errors_are_listed(self):
        text = DRIVERS_DOC.read_text()
        for error in NAMED_ERRORS:
            self.assertIn(error, text, f"named error documented: {error}")


if __name__ == "__main__":
    unittest.main()

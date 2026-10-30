#!/usr/bin/env python3
"""License machine tests — T2 acceptance (plan TC-2.1..TC-2.3).

Red-first: written and observed failing BEFORE tools/deps.py and
tools/package_notices.py existed.

  TC-2.1 one command materializes/verifies all deps at pinned revisions;
         a tampered vendored revision fails with a digest mismatch by name.
  TC-2.2 generated notices cover EVERY file under third_party/ — no
         unlisted blob ships silently.
  TC-2.3 a CC BY-NC-SA component is rejected by name (license gate).

Run: python3 -m unittest discover -s tests -p 'test_*.py' -v
"""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEPS = REPO / "tools" / "deps.py"
NOTICES = REPO / "tools" / "package_notices.py"


def run(tool, *args, **kw):
    return subprocess.run([sys.executable, str(tool), *args],
                          capture_output=True, text=True, **kw)


def sandbox(self):
    """A writable copy of the repo's dependency-managed surfaces."""
    d = Path(tempfile.mkdtemp(prefix="deps-test-"))
    self.addCleanup(shutil.rmtree, d, ignore_errors=True)
    deps_json = json.loads((REPO / "dependencies.json").read_text())
    for entry in deps_json["dependencies"]:
        src = REPO / entry["path"]
        if src.exists():
            shutil.copytree(src, d / entry["path"]
                            if False else d / entry.get("path", entry["path"]))
    (d / "dependencies.json").write_text(json.dumps(deps_json))
    return d


class TestDigestGate(unittest.TestCase):
    """TC-2.1 — pinned revisions verified by digest; tamper is loud."""

    def test_real_tree_verifies(self):
        res = run(DEPS, "verify")
        self.assertEqual(res.returncode, 0, f"{res.stdout}{res.stderr}")

    def test_tampered_tree_fails_by_name(self):
        d = sandbox(self)
        victim = d / "third_party" / "yakumo" / "src" / "iso_image.cpp"
        blob = victim.read_text().replace("ISO9660", "ISO9670", 1)
        self.assertNotEqual(blob, victim.read_text(), "test must actually tamper")
        victim.write_text(blob)
        res = run(DEPS, "verify", "--root", str(d))
        self.assertNotEqual(res.returncode, 0, "tampered vendored tree must fail")
        self.assertIn("DEPS_DIGEST_MISMATCH", res.stdout + res.stderr)

    def test_missing_tree_fails_by_name(self):
        d = sandbox(self)
        shutil.rmtree(d / "third_party" / "yakumo")
        res = run(DEPS, "verify", "--root", str(d))
        self.assertNotEqual(res.returncode, 0, "missing vendored tree must fail")
        self.assertIn("DEPS_MISSING", res.stdout + res.stderr)


class TestNoticeCoverage(unittest.TestCase):
    """TC-2.2 — every third_party file is covered by a generated notice."""

    def test_notices_generated_and_cover_tree(self):
        res = run(NOTICES, "generate")
        self.assertEqual(res.returncode, 0, f"{res.stdout}{res.stderr}")
        text = (REPO / "THIRD_PARTY_NOTICES.txt").read_text()
        self.assertIn("yakumo", text)
        self.assertIn("MIT", text)
        self.assertIn("b61852aef10870400320b96d92a14136ac17b051", text)

    def test_unlisted_blob_rejected_by_name(self):
        d = sandbox(self)
        rogue = d / "third_party" / "stray-blob" / "unattributed.bin"
        rogue.parent.mkdir(parents=True)
        rogue.write_bytes(b"x" * 10)
        res = run(NOTICES, "check-coverage", "--root", str(d))
        self.assertNotEqual(res.returncode, 0, "unlisted third_party blob must fail")
        self.assertIn("UNLISTED_THIRD_PARTY_FILE", res.stdout + res.stderr)
        self.assertIn("stray-blob", res.stdout + res.stderr)

    def test_real_tree_covered(self):
        res = run(NOTICES, "check-coverage")
        self.assertEqual(res.returncode, 0, f"{res.stdout}{res.stderr}")


class TestLicenseGate(unittest.TestCase):
    """TC-2.3 — an incompatible component is rejected by name."""

    def write_entry(self, d, license_name):
        blob_dir = d / "third_party" / "bad-component"
        blob_dir.mkdir(parents=True, exist_ok=True)
        (blob_dir / "component.c").write_text("// placeholder\n")
        (blob_dir / "LICENSE.txt").write_text(f"LICENSE: {license_name}\n")
        deps_json = json.loads((REPO / "dependencies.json").read_text())
        deps_json["dependencies"].append({
            "name": "bad-component", "upstream": "https://example.invalid/x",
            "commit": "0" * 40, "license": license_name,
            "license_file": "third_party/bad-component/LICENSE.txt",
            "path": "third_party/bad-component",
        })
        (d / "dependencies.json").write_text(json.dumps(deps_json))

    def test_cc_by_nc_sa_rejected_by_name(self):
        d = sandbox(self)
        self.write_entry(d, "CC BY-NC-SA 4.0")
        res = run(NOTICES, "check-licenses", "--root", str(d))
        self.assertNotEqual(res.returncode, 0, "CC BY-NC-SA must be rejected")
        combined = res.stdout + res.stderr
        self.assertIn("LICENSE_REJECTED", combined)
        self.assertIn("bad-component", combined)

    def test_approved_licenses_pass(self):
        d = sandbox(self)
        res = run(NOTICES, "check-licenses")
        self.assertEqual(res.returncode, 0, f"{res.stdout}{res.stderr}")


if __name__ == "__main__":
    unittest.main()

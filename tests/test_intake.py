#!/usr/bin/env python3
"""Acceptance tests for tools/intake/ — T3 game intake pipeline (plan TC-3.1..TC-3.4).

Red-first: this suite was written and observed failing (intake tool absent,
C++ reader tool unbuilt) BEFORE the implementation landed.

  TC-3.1 End-to-end on the local game.iso produces manifest.json with the
         expected module set (private input: skipped fail-closed when absent).
  TC-3.2 Wrong-CRC / wrong-size input aborts with a named error.
  TC-3.3 Encrypted, truncated, and garbage ELF inputs are rejected with
         named errors (magic + program-header checks).
  TC-3.4 Every intermediate artifact lands under the output dir;
         repository_check stays green.

Run: python3 -m unittest discover -s tests -p 'test_*.py' -v
"""
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
INTAKE = REPO / "tools" / "intake" / "intake.py"
TOOL = REPO / "build" / "tools" / "intake" / "intake_tool"
GAME_ISO = REPO / "game.iso"
PRIVATE_INPUT = GAME_ISO.exists()


def run(*args):
    return subprocess.run([sys.executable, str(INTAKE), *args],
                          capture_output=True, text=True)


def build_tool():
    """Configure + build the C++ reader tool. Failure here is the expected
    red (implementation absent) for the whole suite."""
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(REPO / "build"),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed (expected red — no CMakeLists yet):\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(REPO / "build"), "--target", "intake_tool"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"cmake build failed (expected red — no tool yet):\n{bld.stderr}")


def setUpModule():
    if not INTAKE.exists():
        raise RuntimeError(f"missing {INTAKE} (expected red — implementation absent)")
    build_tool()


class TestVerifyGate(unittest.TestCase):
    """TC-3.2 — the identity gate (size + CRC32) fails closed, by name."""

    def make_blob(self, name, data):
        d = Path(tempfile.mkdtemp(prefix="intake-test-"))
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        p = d / name
        p.write_bytes(data)
        return p

    def gate(self, iso, size, crc):
        return run("verify", "--iso", str(iso),
                   "--expect-size", str(size), "--expect-crc", crc)

    def test_correct_identity_passes(self):
        data = b"synthetic iso payload for gate test\n"
        p = self.make_blob("ok.iso", data)
        res = self.gate(p, len(data), f"{zlib.crc32(data):08X}")
        self.assertEqual(res.returncode, 0, f"matching identity must pass:\n{res.stdout}{res.stderr}")

    def test_wrong_crc_aborts_by_name(self):
        data = b"synthetic iso payload for gate test\n"
        p = self.make_blob("bad-crc.iso", data)
        res = self.gate(p, len(data), "DEADBEEF")
        self.assertNotEqual(res.returncode, 0, "wrong CRC32 must abort")
        combined = res.stdout + res.stderr
        self.assertIn("IDENTITY_MISMATCH", combined, "failure must be a named error")

    def test_wrong_size_aborts_by_name(self):
        p = self.make_blob("bad-size.iso", b"short")
        res = self.gate(p, 262766592, "183F3EDC")
        self.assertNotEqual(res.returncode, 0, "wrong size must abort")
        self.assertIn("IDENTITY_MISMATCH", res.stdout + res.stderr)

    def test_chd_refused_by_name(self):
        """Disc input is ISO-only; .chd must fail with conversion guidance."""
        p = self.make_blob("disc.chd", b"MComprHD" + b"\x00" * 32)
        res = self.gate(p, 40, "00000000")
        self.assertNotEqual(res.returncode, 0)
        combined = res.stdout + res.stderr
        self.assertIn("CHD_NOT_SUPPORTED", combined, "must name the error and point at conversion docs")
        self.assertIn("CHDStudio", combined)

    @unittest.skipUnless(PRIVATE_INPUT, "needs private game input (game.iso)")
    def test_real_iso_verifies_against_redump(self):
        res = run("verify", "--iso", str(GAME_ISO))
        self.assertEqual(res.returncode, 0, f"real ISO must verify:\n{res.stdout}{res.stderr}")


class TestElfInspection(unittest.TestCase):
    """TC-3.3 — bad module inputs rejected with named errors."""

    def fixture(self, name, data):
        d = Path(tempfile.mkdtemp(prefix="intake-test-"))
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        p = d / name
        p.write_bytes(data)
        return p

    def test_encrypted_psp_module_rejected(self):
        p = self.fixture("eboot.bin", b"~PSP" + b"\x00" * 252)
        res = run("elf-inspect", str(p))
        self.assertNotEqual(res.returncode, 0, "~PSP module must be rejected")
        self.assertIn("ENCRYPTED_EXECUTABLE", res.stdout + res.stderr)

    def test_truncated_elf_rejected(self):
        p = self.fixture("trunc.elf", b"\x7fELF" + b"\x01\x01\x01" + b"\x00" * 26)
        res = run("elf-inspect", str(p))
        self.assertNotEqual(res.returncode, 0, "truncated ELF must be rejected")
        self.assertIn("ELF_INVALID", res.stdout + res.stderr)

    def test_garbage_rejected(self):
        p = self.fixture("junk.bin", b"\x00" * 512)
        res = run("elf-inspect", str(p))
        self.assertNotEqual(res.returncode, 0, "garbage must be rejected")
        self.assertIn("ELF_INVALID", res.stdout + res.stderr)


class TestEndToEnd(unittest.TestCase):
    """TC-3.1 + TC-3.4 — full pipeline on the private disc."""

    @unittest.skipUnless(PRIVATE_INPUT, "needs private game input (game.iso)")
    def test_manifest_and_confinement(self):
        out = REPO / "build" / "test-out"
        if out.exists():
            shutil.rmtree(out)
        status_before = subprocess.run(["git", "status", "--porcelain"],
                                       cwd=REPO, capture_output=True, text=True).stdout

        res = run("run", "--iso", str(GAME_ISO), "--out", str(out))
        self.assertEqual(res.returncode, 0, f"pipeline must succeed:\n{res.stdout}{res.stderr}")

        # TC-3.1: manifest exists with the expected module set.
        manifest = json.loads((out / "manifest.json").read_text())
        self.assertEqual(manifest["disc"]["identity"], "verified")
        disc_id = manifest["sfo"].get("disc_id", "").replace("-", "")
        self.assertEqual(disc_id, "ULUS10592", "PARAM.SFO disc ID must be ULUS-10592")

        modules = manifest["modules"]
        self.assertTrue(modules, "at least EBOOT.BIN must be listed")
        names = {m["name"] for m in modules}
        self.assertIn("EBOOT.BIN", names)
        # Every SYSDIR executable in the inventory must appear in modules.
        sysdir_execs = {f["path"].rsplit("/", 1)[-1] for f in manifest["files"]
                        if f["path"].startswith("PSP_GAME/SYSDIR/")
                        and f["path"].rsplit("/", 1)[-1].lower() in
                        ("eboot.bin", "boot.bin") or f["path"].lower().endswith(".prx")}
        self.assertLessEqual(sysdir_execs, names, "modules must cover SYSDIR executables")

        # Hashes cross-check against independent stdlib computation.
        for f in manifest["files"][:20]:
            local = out / "files" / f["path"]
            digest = hashlib.sha256(local.read_bytes()).hexdigest()
            self.assertEqual(digest, f["sha256"], f"sha256 mismatch for {f['path']}")

        # TC-3.4: artifacts confined to the output dir; tree unchanged otherwise.
        status_after = subprocess.run(["git", "status", "--porcelain"],
                                      cwd=REPO, capture_output=True, text=True).stdout
        self.assertEqual(status_before, status_after, "intake must not create visible files outside build/")
        gate = subprocess.run([sys.executable, str(REPO / "tools" / "repository_check.py")],
                              cwd=REPO, capture_output=True, text=True)
        self.assertEqual(gate.returncode, 0, f"repository_check must stay green:\n{gate.stderr}")


if __name__ == "__main__":
    unittest.main()

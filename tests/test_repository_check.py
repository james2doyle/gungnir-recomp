#!/usr/bin/env python3
"""Acceptance tests for tools/repository_check.py — T1 IP guardrails (plan TC-1.1..TC-1.4).

Red-first: this suite was written and observed failing (tool missing) BEFORE
`tools/repository_check.py` was implemented.

Test cases (plan .dex/plan.md T1):
  TC-1.1 repository_check.py passes on a clean tree.
  TC-1.2 staging a dummy .iso / .elf / ~PSP / .chd blob fails the check (each magic recognized).
  TC-1.3 ignore-rule regression: game paths (e.g. build/game/x.iso) never appear in `git status`.
  TC-1.4 game.chd is never matched by `git ls-files` (untrackable).

Plus a mutation probe: a broken .gitignore must fail the check (proves the
ignore-rule probe works — a green run without it would be vacuous).

Run: python3 -m unittest discover -s tests -p 'test_*.py' -v
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "repository_check.py"


def run(cmd, cwd):
    return subprocess.run(cmd, cwd=str(cwd), capture_output=True, text=True)


class TempRepoMixin:
    """A scratch git repo seeded with our real .gitignore (or a given one)."""

    def make_repo(self, gitignore=None):
        d = tempfile.mkdtemp(prefix="repocheck-test-")
        self.addCleanup(shutil.rmtree, d, ignore_errors=True)
        run(["git", "init", "-q", "-b", "main"], d)
        gi = (REPO / ".gitignore").read_text() if gitignore is None else gitignore
        (pathlib.Path(d) / ".gitignore").write_text(gi)
        return pathlib.Path(d)

    def check(self, repo):
        return run(["python3", str(TOOL), "--repo", str(repo)], repo)


class TestCleanTree(TempRepoMixin, unittest.TestCase):
    """TC-1.1"""

    def test_clean_tree_passes(self):
        repo = self.make_repo()
        (repo / "README.md").write_text("hello\n")
        (repo / "src").mkdir()
        (repo / "src" / "main.c").write_text("int main(void){return 0;}\n")
        run(["git", "add", "-A"], repo)
        res = self.check(repo)
        self.assertEqual(res.returncode, 0, f"clean tree must pass:\n{res.stdout}{res.stderr}")

    def test_work_repo_passes_the_gate(self):
        """TC-1.1 against the real repository: the gate must not false-positive
        on its own source, the tests, or any tracked doc (magic-needle
        self-matches are a live hazard — this reproduces the PE DOS-stub
        self-hit and guards against every future one)."""
        res = self.check(REPO)
        self.assertEqual(
            res.returncode, 0,
            f"the work repo must pass its own gate:\n{res.stdout}{res.stderr}")


class TestForbiddenBlobs(TempRepoMixin, unittest.TestCase):
    """TC-1.2 — each class of forbidden blob must fail the gate."""

    CASES = [
        ("disc.iso", b"not really an iso"),
        ("module.elf", b"\x7fELF\x01\x01\x01" + b"\x00" * 32),
        ("eboot.bin", b"~PSP\x00\x00\x00\x00" + b"\x00" * 32),
        ("disc.chd", b"MComprHD" + b"\x00" * 32),
        ("font.pgf", b"PGF0" + b"\x00" * 32),
        ("archive.dar", b"\x01\x00\x00\x00" * 8),
    ]

    def test_each_forbidden_blob_fails(self):
        for name, data in self.CASES:
            with self.subTest(file=name):
                repo = self.make_repo()
                (repo / name).write_bytes(data)
                # Force-stage: ignore rules normally refuse these outright, but the
                # gate must ALSO catch them when forced in (git add -f, rules edited,
                # imported history) — it is the last line of defense.
                run(["git", "add", "-f", name], repo)
                res = self.check(repo)
                self.assertNotEqual(
                    res.returncode, 0,
                    f"staging {name} must fail the check")
                self.assertIn(
                    name, res.stdout + res.stderr,
                    f"failure must name the offending file {name}")

    def test_oversized_blob_fails(self):
        repo = self.make_repo()
        (repo / "big.txt").write_bytes(b"x" * 1_500_000)
        run(["git", "add", "big.txt"], repo)
        res = self.check(repo)
        self.assertNotEqual(res.returncode, 0, "blobs over the size cap must fail")
        self.assertIn("big.txt", res.stdout + res.stderr)

    def test_secret_material_fails(self):
        repo = self.make_repo()
        # Literal built from fragments so this test file itself never contains it.
        pem = "BEGIN " + "RSA PRIV" + "ATE KEY\n" + "MIIEowIBAAKCAQEA0\n"
        (repo / "notes.txt").write_text("harmless\n" + pem + "\n")
        run(["git", "add", "notes.txt"], repo)
        res = self.check(repo)
        self.assertNotEqual(res.returncode, 0, "private-key material must fail")
        self.assertIn("notes.txt", res.stdout + res.stderr)


class TestIgnoreRules(unittest.TestCase):
    """TC-1.3 / TC-1.4 — game material must be structurally untrackable."""

    def test_game_paths_never_appear_in_status(self):
        with tempfile.TemporaryDirectory(prefix="repocheck-test-") as d:
            repo = pathlib.Path(d)
            run(["git", "init", "-q", "-b", "main"], repo)
            shutil.copy(REPO / ".gitignore", repo / ".gitignore")
            (repo / "build" / "game").mkdir(parents=True)
            (repo / "build" / "game" / "x.iso").write_bytes(b"\x00")
            (repo / "GAME.ISO").write_bytes(b"\x00")
            (repo / "keys").mkdir()
            (repo / "keys" / "psp_keys.txt").write_text("dummy\n")
            (repo / "game.chd").write_bytes(b"MComprHD")
            status = run(["git", "status", "--porcelain"], repo).stdout
            for name in ("x.iso", "GAME.ISO", "psp_keys.txt", "game.chd"):
                self.assertNotIn(name, status, f"{name} must be ignored (invisible to git status)")

    def test_game_chd_untracked_in_work_repo(self):
        # TC-1.4 against the real repo: the local disc must be impossible to commit.
        res = run(["git", "ls-files", "game.chd"], REPO)
        self.assertEqual(res.stdout.strip(), "", "game.chd must never be tracked")
        ign = run(["git", "check-ignore", "-q", "game.chd"], REPO)
        self.assertEqual(ign.returncode, 0, "game.chd must match .gitignore")


class TestIgnoreProbeIsWired(TempRepoMixin, unittest.TestCase):
    """Mutation probe: with an empty .gitignore the gate itself must go red.

    Without this, a green TC-1.1 would be vacuous (the probe could be a no-op).
    """

    def test_broken_gitignore_fails_the_gate(self):
        repo = self.make_repo(gitignore="# intentionally empty\n")
        (repo / "README.md").write_text("hello\n")
        run(["git", "add", "-A"], repo)
        res = self.check(repo)
        self.assertNotEqual(res.returncode, 0, "ignore-rule probes must fail closed")
        combined = res.stdout + res.stderr
        self.assertIn("ignore", combined.lower(), "failure must name the ignore-rule probe")


if __name__ == "__main__":
    unittest.main()

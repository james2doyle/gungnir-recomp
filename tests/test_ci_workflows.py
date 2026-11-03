#!/usr/bin/env python3
"""T13d — CI workflow contract (red-first).

The workflows do not exist yet, so every check fails until they land.
Each check pins one fail-closed property: SHA-pinned actions, strict
private-input mode, synthetic/empty-corpus Android build, least privilege.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOWS = REPO / ".github" / "workflows"

USES = re.compile(r"uses:\s*([^\s#]+)")
PINNED = re.compile(r"^[^@]+@[0-9a-f]{40}$")


def workflow_text():
    texts = {}
    if WORKFLOWS.is_dir():
        for path in sorted(WORKFLOWS.glob("*.yml")):
            texts[path.name] = path.read_text(encoding="utf-8")
    return texts


class TestCiWorkflows(unittest.TestCase):
    def setUp(self):
        self.flows = workflow_text()
        check(self.flows, "no workflows under .github/workflows")

    def test_actions_sha_pinned(self):
        for name, text in self.flows.items():
            for ref in USES.findall(text):
                if ref.startswith("./") or ref.startswith("docker://"):
                    continue
                check(PINNED.match(ref), f"{name} unpinned action: {ref}")

    def test_strict_private_inputs_in_ci(self):
        blob = "\n".join(self.flows.values())
        check("GUNGNIR_REQUIRE_PRIVATE_INPUT" in blob and '"1"' in blob,
              "CI never enables strict private-input mode")

    def test_android_build_uses_no_game_corpus(self):
        blob = "\n".join(self.flows.values())
        check("assembleDebug" in blob or "scripts/android/build.sh" in blob,
              "no Android build step (gradle task or build.sh)")
        for token in ("game.iso", "gameCorpus=", "gameCorpus =", "USRDIR", "profile-test/generated"):
            check(token not in blob, f"CI references private game path: {token}")

    def test_no_secret_bearing_triggers(self):
        blob = "\n".join(self.flows.values())
        check("pull_request_target" not in blob, "pull_request_target must not appear")
        check("secrets." not in blob, "workflows must not consume secrets")

    def test_least_privilege_and_concurrency(self):
        blob = "\n".join(self.flows.values())
        check("permissions:" in blob, "no permissions block")
        check("contents: read" in blob, "permissions are not read-only")
        check("concurrency:" in blob, "no concurrency cancellation")

    def test_hygiene_gates_present(self):
        blob = "\n".join(self.flows.values())
        for gate in ("gitleaks", "package_notices.py check-coverage",
                     "package_notices.py check-licenses", "repository_check"):
            check(gate in blob, f"hygiene gate missing from CI: {gate}")
        check("reuse" in blob and "lint" in blob, "hygiene gate missing from CI: reuse lint")


def check(cond, reason):
    if not cond:
        raise AssertionError(reason)


if __name__ == "__main__":
    unittest.main()

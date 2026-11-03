#!/usr/bin/env python3
"""T14a — QA campaign contract (red-first).

docs/QA.md declares the device matrix; each SIGNED cell must point at an
existing docs/qa/ report carrying the sign-off fields (date, build, APK
hash, device identity, steps, expected/actual table, sign-off line). The
test is red until QA.md exists, red while every cell is PENDING, and goes
green cell by cell as signed reports land. Historical docs/qa/ reports
predating the template are untouched; only SIGNED campaign rows must
match it.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
QA_DOC = REPO / "docs" / "QA.md"
QA_DIR = REPO / "docs" / "qa"

REQUIRED_AREAS = ("cold boot", "suspend/resume", "soak", "touch", "gamepad",
                  "save/load", "audio", "emulator")

REQUIRED_FIELDS = (
    ("date", re.compile(r"^Date:\s*\S+", re.M)),
    ("build", re.compile(r"^Build:\s*\S+", re.M)),
    ("apk hash", re.compile(r"APK SHA-256:\s*`[0-9a-f]{64}`", re.M)),
    ("device model", re.compile(r"\|\s*Model\s*\|", re.M)),
    ("android version", re.compile(r"\|\s*Android\s*\|", re.M)),
    ("gpu/driver", re.compile(r"\|\s*GPU\s*\|", re.M)),
    ("steps", re.compile(r"^#+ Steps", re.M)),
    ("expected/actual", re.compile(r"^#+ Expected and actual", re.M)),
    ("sign-off", re.compile(r"^Sign-off:\s*\S+", re.M)),
)

ROW = re.compile(r"^\|\s*(?P<cell>[^|]+?)\s*\|\s*(?P<report>[^|]*?)\s*\|\s*(?P<status>[^|]*?)\s*\|$", re.M)


def check(cond, reason):
    if not cond:
        raise AssertionError(reason)


class TestQaCampaign(unittest.TestCase):
    def test_matrix_declared(self):
        check(QA_DOC.is_file(), "docs/QA.md missing: matrix not declared")
        text = QA_DOC.read_text(encoding="utf-8")
        rows = [m for m in ROW.finditer(text)
                if "cell" not in m.group("cell").lower()]
        check(rows, "QA.md declares no matrix rows")
        areas = " ".join(m.group("cell") for m in rows).lower()
        for area in REQUIRED_AREAS:
            check(area in areas, f"matrix omits required area: {area}")
        signed = [m for m in rows if m.group("status").strip().upper() == "SIGNED"]
        check(signed, "no SIGNED matrix cell yet: campaign not started")
        problems = []
        for match in signed:
            report = (QA_DIR / match.group("report").strip()).name
            path = QA_DIR / report
            if not path.is_file():
                problems.append(f"{match.group('cell').strip()}: report {report} absent")
                continue
            body = path.read_text(encoding="utf-8")
            for label, pattern in REQUIRED_FIELDS:
                if not pattern.search(body):
                    problems.append(f"{report}: missing {label}")
        check(not problems, "signed cells lack evidence:\n" + "\n".join(problems))


if __name__ == "__main__":
    unittest.main()

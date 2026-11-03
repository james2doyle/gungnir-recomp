#!/usr/bin/env python3
"""T13d — private-input probe for ctest (exit 77 = skip).

Exit 0 when the private game input is present, 77 when absent
(ctest maps it to Skipped via SKIP_RETURN_CODE — an honest skip, never
a pass). Strict mode does not change the code: the probe reports
presence, it never fails a clean clone. Fail-closed enforcement for
real tests lives in private_inputs.require_private_input().
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MARKERS = (REPO / "game.iso", REPO / "build" / "game" / "manifest.json")


def decide(present):
    return 0 if present else 77


def main():
    present = any(path.is_file() for path in MARKERS)
    if present:
        print("private input present")
    else:
        print("private_input_missing: game.iso and build/game/manifest.json absent")
    return decide(present)


if __name__ == "__main__":
    sys.exit(main())

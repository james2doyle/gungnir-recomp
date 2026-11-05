# Gungnir Recomp — public status

Counts, criteria and named gaps only. No game screenshots, dump hashes,
keys, or generated content (root `AGENTS.md` hard limits 0–2).

## Milestones

| ID | Name | State | Evidence |
|---|---|---|---|
| M0 | Repo live, policy enforced | closed | 25 tests; `tools/repository_check.py` PASS; `tools/deps.py` verify OK |
| M1 | Pipeline dry-run on synthetic fixtures (Linux) | closed | 8 test groups in `tests/test_core_fixtures.py`; fixtures under `tests/core/fixtures/` |
| M2 | Game analyzed, AOT generated | closed | census gates in `tests/test_profile.py`; 104 translation units at 16 KiB spans |
| M3 | Bounded boot on Linux desktop | closed | 75 tests; `tests/test_ge_suite.py` TC-7.1–TC-7.4; `tests/test_boot.py` TC-6.2 |
| M4 | Title screen on adb device + menu input | planned | criteria in `data/milestones.json`; QA reports in `docs/qa/` (counts only) |
| M5 | Hardening (audio, saves, texture upscaling, drivers, perf) | planned | — |

M4 exit criteria (see `data/milestones.json`): the documented pipeline in
`docs/BUILDING.md` reaches the title screen plus menu input on the adb
device (TC-13.1); touch and gamepad both navigate the title menu;
bounded-run logs itemize all gaps; a manual QA report is accepted for every
device in the matrix (`docs/QA.md` §8.3), including one Adreno device, with
three consecutive cold boots per device.

M4 is not closed on automation alone: physical-device QA sign-off (T14c)
is required before the ledger records it.

## Named gaps (M5 and beyond)

- Audio complete (ATRAC/SAS) and savedata round-trip on device.
- Texture replacement at Nx with crisp text (T8/T18 tracks).
- Custom-driver session on Adreno (user-supplied zip, fail-closed).
- Rumble mapping decision (T14b): implement or defer with evidence.
- Frame-interpolation artifacts (tracked defect): wrong-partner pairing in
  changing oversize buckets; per-draw identity via a guest-side hook.

## Ratchet rule

Every milestone entry in `data/milestones.json` carries
criterion/measure/evidence/state. A measured figure may rise; it must never
be silently lowered — `tools/milestones.py` (`tests/test_milestones.py`)
fails a lowered figure. Regressions stay visible with their cause.

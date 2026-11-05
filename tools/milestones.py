#!/usr/bin/env python3
"""T13h — milestone ledger ratchet checker.

The ledger (data/milestones.json) records measured figures inside each
milestone's evidence. A figure may rise; it must never silently fall:
check_ratchet(old, new) reports every figure present in both that decreased.
Keys added, removed, or reworded are ignored — only lowerings fail.

lower_one_figure() exists for the red-first contract only
(tests/test_milestones.py): it lowers a single figure so the test can prove
the checker fires.
"""
import copy
import re

FIGURE = re.compile(r"\d+(?:\.\d+)?")


def _figures(value, path=()):
    """Yield (dotted path, index, float) for every number in evidence leaves."""
    if isinstance(value, dict):
        for key, item in value.items():
            yield from _figures(item, path + (str(key),))
    elif isinstance(value, (list, tuple)):
        for idx, item in enumerate(value):
            yield from _figures(item, path + (str(idx),))
    elif isinstance(value, bool):
        return
    elif isinstance(value, (int, float)):
        yield (".".join(path), 0, float(value))
    elif isinstance(value, str):
        for idx, match in enumerate(FIGURE.finditer(value)):
            yield (".".join(path), idx, float(match.group()))


def ledger_figures(ledger):
    """Map (milestone, path, index) -> figure for a parsed ledger."""
    out = {}
    for mid, entry in ledger.get("milestones", {}).items():
        for path, idx, number in _figures(entry.get("evidence", {})):
            out[(mid, path, idx)] = number
    return out


def check_ratchet(old, new):
    """Return a list of human-readable drops; empty means the ratchet holds."""
    before = ledger_figures(old)
    after = ledger_figures(new)
    drops = []
    for key, was in sorted(before.items()):
        if key in after and after[key] < was:
            mid, path, idx = key
            drops.append(
                f"{mid} evidence[{path}]#{idx} lowered: {was:g} -> {after[key]:g}"
            )
    return drops


def lower_evidence_string(text):
    """Return text with its first figure decremented (pure helper)."""
    match = FIGURE.search(text)
    if not match:
        return text
    return text[: match.start()] + ("%g" % (float(match.group()) - 1)) + text[match.end() :]


def _rewrite_first_number(value):
    """Decrement the first figure in an evidence tree in place; True if moved."""
    if isinstance(value, dict):
        for key, item in value.items():
            if isinstance(item, str):
                rewritten = lower_evidence_string(item)
                if rewritten != item:
                    value[key] = rewritten
                    return True
            elif _rewrite_first_number(item):
                return True
        return False
    if isinstance(value, list):
        for idx, item in enumerate(value):
            if isinstance(item, str):
                rewritten = lower_evidence_string(item)
                if rewritten != item:
                    value[idx] = rewritten
                    return True
            elif _rewrite_first_number(item):
                return True
        return False
    return False


def lower_one_figure(ledger):
    """Decrement the first figure found in a closed milestone's evidence.

    Mutates the ledger in place; returns True on success. Test support only.
    """
    snapshot = copy.deepcopy(ledger)
    for mid, entry in ledger.get("milestones", {}).items():
        if entry.get("state") != "closed":
            continue
        if _rewrite_first_number(entry.get("evidence", {})):
            assert snapshot != ledger  # the figure actually moved
            return True
    return False

#!/usr/bin/env python3
"""erqd6wye — snapshot failure must never strand a stopped session.

Device finding (T14a, 2026-11-03): Save state stopped the session, the save
failed, and the failure path returned without relaunching — frozen game,
untouched snapshot file, toast-only error. Contract: every snapshot-op
failure branch in the Android entry point must relaunch the worker
(launch_session_thread) and log the cause (GUNGNIR_LOGE) before returning
the error to the UI.
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"

OPS = ("nativeSaveSnapshot", "nativeLoadSnapshot", "nativeResetToTitle")


def function_body(text, name):
    start = text.find(name)
    if start < 0:
        return ""
    brace = text.find("{", start)
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i]
    return ""


def check(cond, reason):
    if not cond:
        raise AssertionError(reason)


class TestSnapshotRelaunch(unittest.TestCase):
    def test_failure_branches_relaunch_and_log(self):
        text = MAIN.read_text(encoding="utf-8")
        for op in OPS:
            body = function_body(text, op)
            check(body, f"{op} not found")
            failures = [line for line in body.splitlines()
                        if "return env->NewStringUTF(error" in line]
            check(failures, f"{op} has no error-return branch")
            for line in failures:
                pos = body.find(line)
                window = body[max(0, pos - 600):pos]
                check("launch_session_thread" in window,
                      f"{op}: error branch returns without relaunching the session")
                check("GUNGNIR_LOGE" in window,
                      f"{op}: error branch returns without logging the cause")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T15d — snapshot surface contracts (host-checked, no device needed).

The Menu overlay owns save/load rows, the activity drives them through JNI
into the session guarded by the snapshot mutex, the boot runner accepts
--snapshot-in/--snapshot-out, and GameSession exposes save/load/resume.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OVERLAY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "SettingsOverlayView.java"
ACTIVITY = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp" / "GungnirActivity.java"
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
BOOT_RUNNER = REPO / "tests" / "hle" / "boot_runner.cpp"
SESSION_HPP = REPO / "runtime" / "session" / "game_session.hpp"


class TestMenuOverlay(unittest.TestCase):
    def test_menu_has_state_rows_and_no_snapshot_wording(self):
        text = OVERLAY.read_text()
        for needle in ("Save state", "Load state", "Reset to title", "onSaveState",
                        "onLoadState", "onResetToTitle"):
            self.assertIn(needle, text, f"menu overlay is missing {needle}")
        for banned in ("Save snapshot", "Load snapshot"):
            self.assertNotIn(banned, text, f"menu overlay must not say {banned!r}")
        close_at = text.find('"Close"')
        reset_at = text.find('"Reset to title"')
        self.assertGreater(close_at, 0, "menu overlay is missing Close")
        self.assertGreater(reset_at, 0, "menu overlay is missing Reset to title")
        self.assertLess(reset_at, close_at, "Reset to title must come before Close")

    def test_activity_drives_jni_with_app_private_dir(self):
        text = ACTIVITY.read_text()
        for needle in ("nativeSaveSnapshot", "nativeLoadSnapshot", "nativeResetToTitle", "runSnapshotOp",
                        "snapshots", "snapshot.gsnap", "getExternalFilesDir"):
            self.assertIn(needle, text, f"activity is missing {needle}")
        for banned in ("Snapshot saved", "Snapshot loaded"):
            self.assertNotIn(banned, text, f"activity toast must not say {banned!r}")
        self.assertNotIn("MANAGE_EXTERNAL_STORAGE", text)


class TestNativeWiring(unittest.TestCase):
    def test_jni_save_load_guarded_by_mutex(self):
        text = MAIN_CPP.read_text()
        for needle in ("nativeSaveSnapshot", "nativeLoadSnapshot", "nativeResetToTitle", "g_snapshot_mutex",
                        "g_snapshot_session", "launch_session_thread", "request_resume",
                        "restart_to_title", "g_session_rearm", "snapshot_in", "intent_string"):
            self.assertIn(needle, text, f"native layer is missing {needle}")

    def test_session_api_exists(self):
        text = SESSION_HPP.read_text()
        for needle in ("save_snapshot", "load_snapshot", "request_resume", "restart_to_title"):
            self.assertIn(needle, text, f"GameSession is missing {needle}")


class TestBootRunnerFlags(unittest.TestCase):
    def test_snapshot_flags_accepted(self):
        text = BOOT_RUNNER.read_text()
        for needle in ("--snapshot-out", "--snapshot-in", "snapshot_out", "snapshot_in",
                        "load_snapshot", "save_snapshot", "CountingSink", "reset()"):
            self.assertIn(needle, text, f"boot runner is missing {needle}")


if __name__ == "__main__":
    unittest.main()

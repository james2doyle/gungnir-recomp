#!/usr/bin/env python3
"""Background/foreground session lifecycle gate.

Builds and runs the push/pull policy that decides when the guest is stopped on
Android background and resumed in place on foreground, then pins the wiring in
the Android entry point: the stop is requested through the policy, the resume
runs only after the surface is back, and neither path may restart the app.

Red-first: written before runtime/session/session_lifecycle.* existed.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "session" / "session_lifecycle_tests"
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
SESSION_CPP = REPO / "runtime" / "session" / "game_session.cpp"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "session_lifecycle_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"session lifecycle tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSessionLifecyclePolicy(unittest.TestCase):
    def test_policy_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"session lifecycle contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("session_lifecycle_tests: OK", proc.stdout)


class TestAndroidWiring(unittest.TestCase):
    def test_background_stops_through_the_policy(self):
        text = MAIN_CPP.read_text()
        self.assertIn("on_background", text,
                      "the lifecycle watch must ask the policy before stopping the guest")
        start = text.index("SDL_EVENT_WILL_ENTER_BACKGROUND")
        body = text[start:start + 1400]
        self.assertIn("session_stop_requested reason=background", body,
                      "the background stop must stay in the log")
        self.assertLess(body.index("on_background"), body.index("request_stop"),
                        "request_stop must be gated by the policy decision")

    def test_foreground_resumes_in_place(self):
        text = MAIN_CPP.read_text()
        self.assertIn("on_foreground", text,
                      "the foreground event must ask the policy to resume")
        self.assertIn("session_resumed reason=foreground", text,
                      "the resume must be visible in the log")
        start = text.index("session_resumed reason=foreground")
        body = text[max(0, start - 1200):start]
        self.assertIn("request_resume", body,
                      "the resume must continue from the saved guest pc")
        self.assertIn("launch_session_thread", body,
                      "the resume must relaunch the worker thread")

    def test_resume_waits_for_the_surface(self):
        text = MAIN_CPP.read_text()
        start = text.index("SDL_EVENT_DID_ENTER_FOREGROUND")
        body = text[start:start + 900]
        self.assertNotIn("session_resumed", body,
                         "the foreground event must not resume before the surface exists")

    def test_resume_never_revives_an_own_reason_stop(self):
        text = MAIN_CPP.read_text()
        self.assertIn("StopCause::HostStop", text,
                      "only a host-requested stop may be revived after background")
        self.assertIn("g_snapshot_mutex", text,
                      "the join/relaunch must share the snapshot mutex with menu ops")

    def test_resume_rebases_the_pacing_hold(self):
        text = SESSION_CPP.read_text()
        start = text.index("void GameSession::request_resume()")
        body = text[start:start + 400]
        self.assertIn("resync_real_time", body,
                      "a resumed session must forget the wall time spent paused, or "
                      "it either races to catch up or holds against a stale base")

    def test_background_mute_lives_in_the_watch(self):
        """SDL blocks the event pump while backgrounded, so a mute applied by
        the frame loop never runs and the paused stream's ring overruns."""
        text = MAIN_CPP.read_text()
        start = text.index("SDL_EVENT_WILL_ENTER_BACKGROUND")
        body = text[start:start + 2400]
        self.assertIn("audio_muted reason=background", body,
                      "the background mute must log alongside the pause")
        self.assertLess(body.index("audio_paused"), body.index("set_muted(true)"),
                        "the mute must follow the audio pause in the lifecycle watch")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T11f — gamepad navigation for the Menu overlay.

Runs the pure navigation contracts (button mapping, clamp, open/closed gate,
bounded queue) and checks the Java/JNI wiring exists. Device QA with a paired
pad is still required by the task and is not replaced by this gate.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "input" / "menu_navigation_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "menu_navigation_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"menu navigation tests build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestMenuNavigation(unittest.TestCase):
    def test_selection_and_gate(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"menu navigation contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("menu_navigation_tests: OK", proc.stdout)

    def test_android_wiring(self):
        activity = (REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir"
                    / "recomp" / "GungnirActivity.java").read_text()
        overlay = (REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir"
                   / "recomp" / "SettingsOverlayView.java").read_text()
        native = (REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp").read_text()
        android_cmake = (REPO / "cmake" / "AndroidRuntime.cmake").read_text()
        self.assertIn("nativeMenuBegin", activity, "activity begins the model")
        self.assertIn("nativeMenuEnd", activity, "activity ends the model")
        self.assertIn("nativeMenuPoll", activity, "activity polls the model")
        self.assertIn("GungnirActivity_nativeMenuPoll", native, "native poll is exported")
        self.assertIn("runtime/input/menu_navigation.cpp", android_cmake,
                      "the Android runtime compiles the navigation model")
        self.assertIn("setSelection", overlay, "overlay exposes selection")
        self.assertIn("activateSelection", overlay, "overlay activates through the Listener path")

    def test_docs_cover_menu_navigation(self):
        doc = (REPO / "docs" / "INPUT.md").read_text()
        self.assertIn("## Menu navigation", doc)
        self.assertIn("clamp", doc.lower())


if __name__ == "__main__":
    unittest.main()

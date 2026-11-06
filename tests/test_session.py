#!/usr/bin/env python3
"""T13a — shared session report and Android runtime wiring contracts.

The report binary checks stop-cause classification, the summary/session_stop
line and the boot JSON. The static contracts check that the Android target
compiles the same session/HLE/audio/savedata sources as the desktop runtime,
that the AOT corpus is a build-time input (never tracked), and that the app
wires the memstick, NID registry and session log.
"""
import json
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "session" / "session_report_tests"
ANDROID_CMAKE = REPO / "cmake" / "AndroidRuntime.cmake"
ANDROID_MAIN = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
APP_GRADLE = REPO / "android" / "app" / "build.gradle.kts"
RUN_SH = REPO / "scripts" / "android" / "run.sh"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "session_report_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"session report test build failed:\n"
                           f"{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSessionReport(unittest.TestCase):
    def test_report_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=60)
        self.assertEqual(proc.returncode, 0, f"{proc.stdout}{proc.stderr}")
        self.assertIn("session_report_tests: OK", proc.stdout)


class TestAndroidRuntimeWiring(unittest.TestCase):
    def test_cmake_compiles_the_shared_chain(self):
        text = ANDROID_CMAKE.read_text()
        for source in (
            "runtime/session/game_session.cpp",
            "runtime/session/diagnostics.cpp",
            "runtime/session/import_stub.cpp",
            "runtime/session/session_report.cpp",
            "runtime/session/snapshot.cpp",
            "runtime/hle/hle_common.cpp",
            "runtime/hle/kernel.cpp",
            "runtime/hle/hle_io.cpp",
            "runtime/hle/hle_utility.cpp",
            "runtime/hle/hle_atrac.cpp",
            "runtime/hle/hle_audio.cpp",
            "runtime/hle/hle_media.cpp",
            "runtime/hle/hle_module.cpp",
            "runtime/audio/atrac_file.cpp",
            "runtime/savedata/save_store.cpp",
            "third_party/yakumo/src/elf32.cpp",
            "third_party/yakumo/src/iso_image.cpp",
            "third_party/yakumo/src/param_sfo.cpp",
            "third_party/atrac3p/atrac3p_api.c",
        ):
            self.assertIn(source, text, f"Android target is missing {source}")

    def test_corpus_is_a_build_input_with_a_named_absence(self):
        text = ANDROID_CMAKE.read_text()
        self.assertIn("GUNGNIR_GAME_CORPUS", text)
        self.assertIn("generated_unit_*.cpp", text)
        self.assertIn("game_corpus_missing", text)
        self.assertIn("corpus_present.cpp", text)
        self.assertIn("corpus_absent.cpp", text)
        gradle = APP_GRADLE.read_text()
        self.assertIn("gungnir.gameCorpus", gradle)
        self.assertIn("-DGUNGNIR_GAME_CORPUS=", gradle)

    def test_app_wires_session_memstick_and_registry(self):
        text = ANDROID_MAIN.read_text()
        for needle in ("session::GameSession", "session_stop", "memory_stick",
                       "nids.csv", "game.elf", "set_audio_sink"):
            self.assertIn(needle, text, f"Android entry point does not wire {needle}")

    def test_corpus_absence_is_a_named_interpreter_mode(self):
        # k9ll558m: a corpus-absent build must boot on the interpreter
        # fallback, never refuse and never run silent. The session entry
        # names the mode; the old game_corpus_missing refusal must be gone.
        text = (REPO / "runtime" / "session" / "game_session.cpp").read_text()
        self.assertIn("interpreter_only_build", text,
                      "corpus-absent start must log its named mode")
        self.assertNotIn("game_corpus_missing: rebuild with",
                         text, "the corpus-absent refusal is retired")

    def test_run_script_waits_for_the_session_line(self):
        text = RUN_SH.read_text()
        self.assertIn("session_stop", text)
        self.assertIn("cause=first_flip", text)
        self.assertIn("missing_import=0", text)


if __name__ == "__main__":
    unittest.main()

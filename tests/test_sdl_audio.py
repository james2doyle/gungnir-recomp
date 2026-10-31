#!/usr/bin/env python3
"""T12a — SDL3 audio backend smoke (host).

Skips with a named reason when SDL3 or a usable audio device is absent.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "audio" / "sdl_audio_smoke"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise unittest.SkipTest(f"cmake configure failed: {cfg.stderr[-1000:]}")
    targets = subprocess.run(["cmake", "--build", str(BUILD), "--target", "help"],
                             capture_output=True, text=True)
    if "sdl_audio_smoke" not in targets.stdout:
        raise unittest.SkipTest("sdl_audio_smoke not built (SDL3 missing)")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "sdl_audio_smoke"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"SDL audio smoke build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestSdlAudio(unittest.TestCase):
    def test_sdl_audio_smoke(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        output = proc.stdout + proc.stderr
        if proc.returncode == 77:
            self.skipTest(output.strip() or "sdl_audio_unavailable")
        self.assertEqual(proc.returncode, 0, f"SDL audio smoke failed:\n{output}")
        self.assertIn("sdl_audio_smoke: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""T12a — audio mixer contracts (ring, volumes, underrun/overflow).

Red-first: audio_mixer.* and its target did not exist when this test was
written.
"""
import subprocess
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
BINARY = BUILD / "tests" / "audio" / "audio_mixer_tests"


def setUpModule():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "audio_mixer_tests"],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"audio mixer build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


class TestAudioMixer(unittest.TestCase):
    def test_mixer_contracts(self):
        proc = subprocess.run([str(BINARY)], capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0,
                         f"audio mixer contracts failed:\n{proc.stdout}{proc.stderr}")
        self.assertIn("audio_mixer_tests: OK", proc.stdout)


if __name__ == "__main__":
    unittest.main()

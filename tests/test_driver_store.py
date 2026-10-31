#!/usr/bin/env python3
"""T10d — pure-JVM driver inventory, selection file and delete contracts."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SOURCE = REPO / "android" / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp"
TEST = REPO / "android" / "app" / "src" / "test" / "java" / "com" / "gungnir" / "recomp"
JAVAC = shutil.which("javac")
JAVA = shutil.which("java")


class TestDriverStore(unittest.TestCase):
    def test_driver_store_contracts(self):
        if JAVAC is None or JAVA is None:
            self.skipTest("no JDK javac/java on PATH")
        with tempfile.TemporaryDirectory(prefix="gungnir-driver-store-") as directory:
            classes = Path(directory)
            compile_result = subprocess.run(
                [JAVAC, "-d", str(classes),
                 str(SOURCE / "DriverStore.java"),
                 str(TEST / "DriverStoreTest.java")],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            run_result = subprocess.run(
                [JAVA, "-cp", str(classes), "com.gungnir.recomp.DriverStoreTest"],
                capture_output=True, text=True, timeout=120)
            output = run_result.stdout + run_result.stderr
            self.assertEqual(run_result.returncode, 0, f"driver store failed:\n{output}")
            self.assertIn("driver_store_tests: OK", run_result.stdout)


if __name__ == "__main__":
    unittest.main()

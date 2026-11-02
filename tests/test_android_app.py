#!/usr/bin/env python3
"""T9b — SDL3 pin, fetch reuse and Android shell contracts."""
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEPS_CMAKE = REPO / "cmake" / "Deps.cmake"
ARCHIVE = REPO / "build" / "android-deps" / "SDL3-3.4.18.tar.gz"


class TestSdl3Dependency(unittest.TestCase):
    def test_manifest_and_cmake_agree(self):
        data = json.loads((REPO / "dependencies.json").read_text())
        sdl = next(entry for entry in data["dependencies"] if entry["name"] == "sdl3")
        self.assertEqual(sdl["type"], "fetch")
        self.assertEqual(sdl["version"], "3.4.18")
        self.assertEqual(sdl["license"], "Zlib")
        cmake = DEPS_CMAKE.read_text()
        self.assertIn(f'SDL3_VERSION "{sdl["version"]}"', cmake)
        self.assertIn("libsdl-org/SDL/releases/download/release-${SDL3_VERSION}", cmake)
        self.assertIn(f'SDL3_SHA256 "{sdl["archive_sha256"]}"', cmake)

    def test_fetch_reuses_verified_archive(self):
        if not ARCHIVE.is_file():
            self.skipTest("SDL3 archive not materialized on this host")
        with tempfile.TemporaryDirectory(prefix="gungnir-sdl3-") as directory:
            deps = Path(directory)
            shutil.copy2(ARCHIVE, deps / ARCHIVE.name)
            command = ["cmake", f"-DGUNGNIR_DEPS_DIR={deps}", "-P", str(DEPS_CMAKE)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertTrue((deps / "sdl3-src" / "CMakeLists.txt").is_file())
            self.assertTrue((deps / "sdl3-src" / "android-project" / "app" /
                             "src" / "main" / "java" / "org" / "libsdl" / "app" /
                             "SDLActivity.java").is_file())
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertIn("cached", second.stdout)

    def test_deps_verifier_rejects_tampered_archive(self):
        if not ARCHIVE.is_file():
            self.skipTest("SDL3 archive not materialized on this host")
        data = json.loads((REPO / "dependencies.json").read_text())
        sdl = next(entry for entry in data["dependencies"] if entry["name"] == "sdl3")
        with tempfile.TemporaryDirectory(prefix="gungnir-deps-") as directory:
            root = Path(directory)
            (root / "build" / "android-deps").mkdir(parents=True)
            (root / "build" / "android-deps" / ARCHIVE.name).write_bytes(b"tampered")
            (root / "dependencies.json").write_text(json.dumps({"dependencies": [sdl]}))
            proc = subprocess.run(["python3", str(REPO / "tools" / "deps.py"), "verify",
                                   "--root", str(root)], capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("DEPS_DIGEST_MISMATCH", proc.stdout + proc.stderr)

    def test_tampered_archive_is_rejected(self):
        if not ARCHIVE.is_file():
            self.skipTest("SDL3 archive not materialized on this host")
        with tempfile.TemporaryDirectory(prefix="gungnir-sdl3-bad-") as directory:
            deps = Path(directory)
            corrupted = deps / ARCHIVE.name
            corrupted.write_bytes(ARCHIVE.read_bytes()[:1024])
            proc = subprocess.run(["cmake", f"-DGUNGNIR_DEPS_DIR={deps}",
                                   "-DGUNGNIR_OFFLINE=ON", "-P", str(DEPS_CMAKE)],
                                  capture_output=True, text=True)
            self.assertNotEqual(proc.returncode, 0, "tampered SDL3 archive must fail")


class TestDriverDependencies(unittest.TestCase):
    def test_adrenotools_fetch_pins_match_the_script(self):
        data = json.loads((REPO / "dependencies.json").read_text())
        entries = {entry["name"]: entry for entry in data["dependencies"]}
        script = (REPO / "cmake" / "AdrenotoolsDeps.cmake").read_text()
        for name in ("libadrenotools", "liblinkernsbypass"):
            entry = entries[name]
            self.assertEqual(entry["type"], "fetch")
            self.assertIn(entry["commit"], script)
            self.assertIn(entry["archive_sha256"], script)

    def test_volk_is_vendored_and_built_from_the_repo(self):
        data = json.loads((REPO / "dependencies.json").read_text())
        volk = next(entry for entry in data["dependencies"] if entry["name"] == "volk")
        self.assertEqual(volk["type"], "vendored")
        self.assertEqual(volk["license"], "MIT")
        runtime = (REPO / "cmake" / "AndroidRuntime.cmake").read_text()
        self.assertIn("third_party/volk/volk.c", runtime)
        self.assertNotIn("GUNGNIR_VOLK_DIR", runtime)
        android = (REPO / "android" / "app" / "CMakeLists.txt").read_text()
        self.assertNotIn("GUNGNIR_VOLK_DIR", android)


class TestAndroidScripts(unittest.TestCase):
    def test_build_help_documents_offline(self):
        proc = subprocess.run([str(REPO / "scripts" / "android" / "build.sh"), "--help"],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("--offline", proc.stdout)

    def test_android_agents_is_scoped_and_lean(self):
        text = (REPO / "android" / "AGENTS.md").read_text()
        self.assertIn("Root `AGENTS.md` is authoritative", text)
        self.assertIn("scripts/android/emulator.sh", text)
        self.assertIn("-wipe-data", text)
        self.assertIn("physical arm64", text)
        self.assertIn("docs/ANDROID_DRIVERS.md", text)
        root = (REPO / "AGENTS.md").read_text()
        self.assertIn("`android/AGENTS.md`", root)

    def test_emulator_script_is_reproducible(self):
        script = (REPO / "scripts" / "android" / "emulator.sh").read_text()
        for flag in ("-avd", "-port", "-no-window", "-no-audio", "-no-boot-anim",
                     "swiftshader_indirect", "-no-snapshot", "-wipe-data",
                     "-netdelay none", "-netspeed full"):
            self.assertIn(flag, script, f"emulator flag recorded: {flag}")
        self.assertIn("sys.boot_completed", script, "boot completion is awaited")
        self.assertIn("emulator-", script, "the serial is reported")

    def test_deploy_scripts_exist(self):
        scripts = REPO / "scripts" / "android"
        for name in ("build.sh", "deploy.sh", "install.sh", "run.sh", "logcat.sh", "lifecycle.sh",
                     "screenshot.sh", "emulator.sh"):
            path = scripts / name
            self.assertTrue(path.is_file(), f"{name} exists")
            self.assertTrue(path.stat().st_mode & 0o111, f"{name} is executable")


if __name__ == "__main__":
    unittest.main()

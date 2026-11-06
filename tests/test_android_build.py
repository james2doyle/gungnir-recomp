#!/usr/bin/env python3
"""T9a/T9b — Android build skeleton, pins and clean-checkout APK contracts.

The APK build is skipped by name when no usable Android SDK is present. The
static contract checks (pins, no machine-local paths, named failures) always
run.
"""
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ANDROID = REPO / "android"
BUILD_SH = REPO / "scripts" / "android" / "build.sh"
GRADLEW = ANDROID / "gradlew"
WRAPPER = ANDROID / "gradle" / "wrapper" / "gradle-wrapper.properties"
SDK = Path(os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT") or "")

DIST_SHA256 = "f397b287023acdba1e9f6fc5ea72d22dd63669d59ed4a289a29b1a76eee151c6"
FORBIDDEN_EXTENSIONS = (".iso", ".chd", ".cso", ".elf", ".prx", ".dar", ".tpl",
                        ".zip", ".apk", ".jar", ".aab")


def usable_java_home():
    homes = []
    configured = os.environ.get("JAVA_HOME")
    if configured:
        homes.append(Path(configured))
    jvm_root = Path("/usr/lib/jvm")
    if jvm_root.is_dir():
        homes.extend(path.parent.parent for path in sorted(jvm_root.glob("*/bin/java")))
    for home in homes:
        java = home / "bin" / "java"
        if not java.is_file():
            continue
        proc = subprocess.run([str(java), "-version"], capture_output=True, text=True)
        match = re.search(r'version "([0-9]+)', proc.stdout + proc.stderr)
        if match and 17 <= int(match.group(1)) <= 23:
            return home
    return None


class TestAndroidBuildSkeleton(unittest.TestCase):
    def test_activity_surface_is_opaque(self):
        # The guest clears alpha to zero over fullscreen regions (CLEAR mode
        # alpha-only passes); a translucent SurfaceView would composite those
        # regions to black. The activity must force the SDL surface opaque.
        activity = (ANDROID / "app" / "src" / "main" / "java" / "com" / "gungnir" / "recomp"
                    / "GungnirActivity.java").read_text()
        self.assertIn("createSDLSurface", activity, "activity owns the SDL surface creation")
        self.assertIn("PixelFormat.OPAQUE", activity, "surface holder forced opaque")

    def test_wrapper_is_sha_pinned(self):
        self.assertTrue(WRAPPER.is_file(), "gradle-wrapper.properties exists")
        text = WRAPPER.read_text()
        self.assertIn("distributionUrl=", text)
        self.assertIn("gradle-8.11.1-bin.zip", text)
        self.assertIn(f"distributionSha256Sum={DIST_SHA256}", text)
        self.assertTrue(GRADLEW.is_file(), "gradlew bootstrap exists")
        self.assertTrue(os.access(GRADLEW, os.X_OK), "gradlew is executable")

    def test_toolchain_pins(self):
        root = (ANDROID / "build.gradle.kts").read_text()
        self.assertIn('version "8.10.1"', root)
        app = (ANDROID / "app" / "build.gradle.kts").read_text()
        for pin in ('compileSdk = 36', 'buildToolsVersion = "36.0.0"',
                    'ndkVersion = "28.2.13676358"', "minSdk = 26", "targetSdk = 35",
                    'version = "3.22.1"', '"arm64-v8a"', '"x86_64"',
                    "-DANDROID_STL=c++_static", "-DCMAKE_CXX_STANDARD=20"):
            self.assertIn(pin, app, f"missing pin: {pin}")

    def test_no_machine_local_paths_tracked(self):
        self.assertFalse((ANDROID / "local.properties").exists(),
                         "no local.properties in the tree")
        for path in (ANDROID / "settings.gradle.kts", ANDROID / "build.gradle.kts",
                     ANDROID / "app" / "build.gradle.kts", BUILD_SH):
            self.assertTrue(path.is_file(), f"{path.name} exists")
            text = path.read_text()
            self.assertNotIn("/opt/android-sdk", text, "no absolute SDK path")
            self.assertNotIn("/home/", text, "no absolute home path")

    def test_gitignore_covers_android_outputs(self):
        text = (REPO / ".gitignore").read_text()
        for pattern in ("local.properties", ".gradle/", ".cxx/", "build/"):
            self.assertIn(pattern, text, f"ignore rule for {pattern}")

    def test_build_help_documents_properties(self):
        proc = subprocess.run([str(BUILD_SH), "--help"], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("-Pgungnir.sdkDir", proc.stdout)
        self.assertIn("-Pgungnir.javaHome", proc.stdout)

    def test_missing_sdk_is_a_named_error(self):
        proc = subprocess.run([str(BUILD_SH), "-Pgungnir.sdkDir=/nonexistent/sdk"],
                              capture_output=True, text=True)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("missing_sdk", proc.stderr)

    def test_clean_checkout_builds_apk(self):
        if not SDK.is_dir():
            self.skipTest("android SDK not installed")
        required = [SDK / "platforms" / "android-36", SDK / "build-tools" / "36.0.0",
                    SDK / "ndk" / "28.2.13676358", SDK / "cmake" / "3.22.1"]
        if not all(path.is_dir() for path in required):
            self.skipTest("android SDK lacks the pinned components")
        java_home = usable_java_home()
        if java_home is None:
            self.skipTest("no JDK 17-23 available for AGP 8.10")
        with tempfile.TemporaryDirectory(prefix="gungnir-android-") as directory:
            root = Path(directory)
            ignore = shutil.ignore_patterns("build", ".cxx", ".gradle", "local.properties")
            shutil.copytree(ANDROID, root / "android", ignore=ignore)
            (root / "scripts" / "android").mkdir(parents=True)
            shutil.copy2(BUILD_SH, root / "scripts" / "android" / "build.sh")
            shutil.copytree(REPO / "cmake", root / "cmake")
            shutil.copytree(REPO / "runtime" / "audio", root / "runtime" / "audio")
            shutil.copytree(REPO / "runtime" / "ge", root / "runtime" / "ge")
            shutil.copytree(REPO / "runtime" / "host", root / "runtime" / "host")
            shutil.copytree(REPO / "runtime" / "hle", root / "runtime" / "hle")
            shutil.copytree(REPO / "runtime" / "input", root / "runtime" / "input")
            shutil.copytree(REPO / "runtime" / "platform", root / "runtime" / "platform")
            shutil.copytree(REPO / "runtime" / "savedata", root / "runtime" / "savedata")
            shutil.copytree(REPO / "runtime" / "session", root / "runtime" / "session")
            shutil.copytree(REPO / "configs", root / "configs")
            shutil.copytree(REPO / "third_party" / "atrac3p", root / "third_party" / "atrac3p")
            shutil.copytree(REPO / "third_party" / "miniz", root / "third_party" / "miniz")
            shutil.copytree(REPO / "third_party" / "nlohmann-json",
                            root / "third_party" / "nlohmann-json")
            (root / "third_party" / "yakumo" / "include").parent.mkdir(parents=True)
            shutil.copytree(REPO / "third_party" / "yakumo" / "include",
                            root / "third_party" / "yakumo" / "include")
            shutil.copytree(REPO / "third_party" / "yakumo" / "src",
                            root / "third_party" / "yakumo" / "src")
            shutil.copytree(REPO / "third_party" / "tiny-aes-c",
                            root / "third_party" / "tiny-aes-c")
            (root / "third_party" / "volk").mkdir(parents=True)
            shutil.copy2(REPO / "third_party" / "volk" / "volk.c",
                         root / "third_party" / "volk" / "volk.c")
            shutil.copy2(REPO / "third_party" / "volk" / "volk.h",
                         root / "third_party" / "volk" / "volk.h")
            shutil.copytree(REPO / "assets" / "shaders" / "ge", root / "assets" / "shaders" / "ge")
            (root / "tools").mkdir()
            shutil.copy2(REPO / "tools" / "embed_shaders.py", root / "tools" / "embed_shaders.py")
            proc = subprocess.run([str(root / "scripts" / "android" / "build.sh"),
                                   f"-Pgungnir.sdkDir={SDK}",
                                   f"-Pgungnir.javaHome={java_home}",
                                   f"-Pgungnir.depsDir={REPO / 'build' / 'android-deps'}",
                                   f"-Pgungnir.o1Sources={root / 'runtime' / 'ge' / 'vertex.cpp'}"],
                                  capture_output=True, text=True, timeout=900)
            self.assertEqual(proc.returncode, 0,
                             f"build failed:\n{proc.stdout[-4000:]}{proc.stderr[-4000:]}")
            apk = (root / "android" / "app" / "build" / "outputs" / "apk" /
                   "debug" / "app-debug.apk")
            self.assertTrue(apk.is_file(), "debug APK exists")
            self.assertGreater(apk.stat().st_size, 4096, "APK is not empty")
            with zipfile.ZipFile(apk) as archive:
                names = archive.namelist()
                dex_bytes = b"".join(archive.read(name) for name in names
                                     if name.startswith("classes") and name.endswith(".dex"))
                libmain = archive.read("lib/arm64-v8a/libmain.so")
            expected_libs = {f"lib/{abi}/libmain.so" for abi in ("arm64-v8a", "x86_64")}
            expected_libs |= {
                "lib/arm64-v8a/libmain_hook.so",
                "lib/arm64-v8a/libfile_redirect_hook.so",
                "lib/arm64-v8a/libgsl_alloc_hook.so",
                "lib/arm64-v8a/libhook_impl.so",
            }
            self.assertTrue(expected_libs.issubset(names), "both ABI runtime libraries exist")
            self.assertNotIn("lib/arm64-v8a/libSDL3.so", names, "SDL3 is linked statically")
            for hook in ("libmain_hook.so", "libfile_redirect_hook.so",
                         "libgsl_alloc_hook.so", "libhook_impl.so"):
                self.assertIn(f"lib/arm64-v8a/{hook}", names, f"hook {hook} is packaged")
            self.assertFalse(any(name.startswith("lib/x86_64/") and "hook" in name for name in names),
                             "x86_64 is system-only")
            self.assertIn(b"org/libsdl/app/SDLActivity", dex_bytes, "SDL Java layer is packaged")
            self.assertIn(b"com/gungnir/recomp/GungnirActivity", dex_bytes,
                          "the app Activity is packaged")
            self.assertIn(b"libmain", dex_bytes, "SDLActivity loads libmain")
            self.assertIn("assets/runtime/version.txt", names, "runtime asset stamp is packaged")
            if shutil.which("readelf"):
                lib_path = root / "libmain.so"
                lib_path.write_bytes(libmain)
                symbols = subprocess.run(["readelf", "-Ws", str(lib_path)],
                                         capture_output=True, text=True)
                self.assertIn("SDL_main", symbols.stdout, "libmain exports SDL_main")

            commands = list((root / "android" / "app" / ".cxx").rglob("compile_commands.json"))
            self.assertTrue(commands, "CMake compile commands exist")
            entries = json.loads(commands[0].read_text())
            def flags(suffix):
                entry = next(item for item in entries if item["file"].endswith(suffix))
                arguments = entry.get("arguments") or shlex.split(entry["command"])
                return arguments
            vertex = flags("vertex.cpp")
            backend = flags("vulkan_backend.cpp")
            self.assertIn("-O2", backend, "runtime TUs compile at -O2 in debug")
            self.assertIn("-fno-lto", backend, "LTO is disabled for the runtime target")
            self.assertIn("-O2", vertex, "runtime TUs still default to -O2")
            self.assertIn("-O1", vertex, "the named override compiles at -O1")
            self.assertGreater(vertex.index("-O1"), vertex.index("-O2"),
                               "the per-unit -O1 override wins over the target -O2")
        for name in names:
            if name.startswith("lib/") and name.endswith(".so"):
                self.assertIn(name, expected_libs, f"unexpected native library: {name}")
            else:
                self.assertFalse(name.lower().endswith(FORBIDDEN_EXTENSIONS),
                                 f"no game/driver blob in the APK: {name}")


if __name__ == "__main__":
    unittest.main()

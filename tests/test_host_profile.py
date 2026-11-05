#!/usr/bin/env python3
"""75yfmuis — per-title host profile wiring contracts.

The profile is opt-in host state: read from a gitignored app-private path at
boot, every applied entry logged, invalid values refused with a named error,
and never part of snapshot identity. These are source contracts for the parts
that cannot be unit-tested from C++ (boot wiring, path, logging).

Red-first: written before the Android wiring existed.
"""
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MAIN_CPP = REPO / "android" / "app" / "src" / "main" / "cpp" / "main.cpp"
ANDROID_CMAKE = REPO / "cmake" / "AndroidRuntime.cmake"
HOST_CMAKE = REPO / "CMakeLists.txt"
PROFILE_HPP = REPO / "runtime" / "host" / "profile.hpp"
GITIGNORE = REPO / ".gitignore"


class TestHostProfileWiring(unittest.TestCase):
    def test_boot_reads_the_gitignored_local_profile(self):
        main = MAIN_CPP.read_text()
        self.assertIn("load_host_profile", main,
                      "the Android shell must load the profile at boot")
        self.assertIn('"profiles" / "gungnir" / "local" / "host_patches.toml"', main,
                      "values must come from the gitignored per-title local dir")
        self.assertIn("profile absent", main,
                      "a missing profile is the normal case, logged as absent")
        self.assertIn("profile_invalid:", main,
                      "a present-but-invalid profile is a named error, not a default")
        self.assertIn("g_host_profile = result.profile", main,
                      "parsed entries become the session's host state")

    def test_every_applied_entry_is_logged(self):
        main = MAIN_CPP.read_text()
        self.assertIn("for (const std::string &line : g_host_profile.applied_lines())", main,
                      "boot must log one line per applied entry")
        self.assertIn('GUNGNIR_LOGI("%s\\n", line.c_str());', main,
                      "each applied line reaches the log verbatim")
        self.assertIn("profile applied=", main)

    def test_profile_never_enters_the_snapshot(self):
        header = PROFILE_HPP.read_text()
        self.assertIn("snapshot_payload()", header,
                      "the profile must declare it contributes no snapshot bytes")
        self.assertIn("not part of snapshot identity", header,
                      "the policy must be stated in the type")
        for path in ("runtime/session/snapshot.cpp", "runtime/session/game_session.cpp"):
            source = (REPO / path).read_text()
            self.assertNotIn("host/profile.hpp", source,
                             f"{path} must not pull host patches into the snapshot path")

    def test_profile_source_is_built_everywhere(self):
        self.assertIn("runtime/host/profile.cpp", HOST_CMAKE.read_text(),
                      "the host CMake must build the profile parser")
        self.assertIn("runtime/host/profile.cpp", ANDROID_CMAKE.read_text(),
                      "the Android runtime CMake must build the profile parser too")

    def test_local_profile_values_are_gitignored(self):
        self.assertIn("profiles/*/local/", GITIGNORE.read_text(),
                      "per-title local values must never be committed")


if __name__ == "__main__":
    unittest.main()

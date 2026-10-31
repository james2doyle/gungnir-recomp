#!/usr/bin/env python3
"""T6c acceptance — bounded real-module boot to the first display flip.

TC-6.2: the recompiled main module runs from its entry through module init to
the first real `sceDisplaySetFrameBuf`, with the adapted kernel/HLE boot set,
under explicit dispatch and wall-clock bounds. Every import that executes is
bound; an unknown import would have stopped the run with its NID, so the
report must name none.

Private input: needs the decrypted main module (build/game/manifest.json) and
the T5 generated corpus (build/profile-test/generated). The suite is skipped
when either is absent.
"""
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
WORK = BUILD / "profile-test"
GENERATED = WORK / "generated"
MANIFEST = BUILD / "game" / "manifest.json"
GAME_ISO = REPO / "game.iso"
NIDS = REPO / "configs" / "nids.csv"
BOOT_SOURCE = REPO / "tests" / "hle" / "boot_runner.cpp"


def game_elf():
    if not MANIFEST.exists():
        return None
    manifest = json.loads(MANIFEST.read_text())
    module = next((m for m in manifest["modules"] if m.get("module_name") == "9th"), None)
    return MANIFEST.parent / module["path"] if module else None


def have_inputs():
    elf = game_elf()
    return bool(elf and elf.is_file() and GAME_ISO.exists()
                and (GENERATED / "generated_registry.cpp").exists())


@unittest.skipUnless(have_inputs(), "needs private game input and the T5 generated corpus")
class TestBoundedBoot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.obj_dir = WORK / "obj"
        cls.obj_dir.mkdir(parents=True, exist_ok=True)
        ccache = shutil.which("ccache")
        cls.compiler = [ccache, "g++"] if ccache else ["g++"]
        cls.include = ["-I", str(REPO / "runtime"),
                       "-I", str(REPO / "third_party" / "yakumo" / "include"),
                       "-I", str(REPO / "third_party" / "yakumo" / "src"),
                       "-I", str(GENERATED)]
        cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                              "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                             capture_output=True, text=True)
        if cfg.returncode != 0:
            raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
        bld = subprocess.run(["cmake", "--build", str(BUILD), "--target", "gungnir_hle"],
                             capture_output=True, text=True)
        if bld.returncode != 0:
            raise RuntimeError(f"gungnir_hle build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")

        # Object files must also be rebuilt when a runtime or framework
        # header changes: the generated units include them, and a stale object
        # linked against a newer library is an ABI mismatch, not a cache hit.
        header_roots = [REPO / "runtime", REPO / "third_party" / "yakumo" / "include",
                        REPO / "third_party" / "yakumo" / "src", GENERATED]
        headers = [path for root in header_roots for path in root.rglob("*.hpp")]
        headers += [path for root in header_roots for path in root.rglob("*.h")]
        newest_header = max((path.stat().st_mtime for path in headers), default=0.0)

        def up_to_date(obj, source):
            return obj.exists() and obj.stat().st_mtime >= source.stat().st_mtime \
                and obj.stat().st_mtime >= newest_header

        sources = sorted(GENERATED.glob("generated_unit_*.cpp"))
        if not sources:
            raise unittest.SkipTest("generated corpus is empty")

        def compile_unit(src):
            obj = cls.obj_dir / (src.stem + ".o")
            if up_to_date(obj, src):
                return None
            proc = subprocess.run([*cls.compiler, "-std=c++20", "-O0", "-g0",
                                   *cls.include, "-c", str(src), "-o", str(obj)],
                                  capture_output=True, text=True)
            return (src.name, proc.returncode, proc.stderr[-2000:])

        with ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 4)) as pool:
            results = [r for r in pool.map(compile_unit, sources) if r is not None]
        failures = [(name, err) for name, rc, err in results if rc != 0]
        if failures:
            raise RuntimeError(f"generated TU compile failures: {failures[:3]}")

        registry = GENERATED / "generated_registry.cpp"
        registry_obj = cls.obj_dir / "generated_registry.o"
        if not up_to_date(registry_obj, registry):
            proc = subprocess.run([*cls.compiler, "-std=c++20", "-O0", "-g0",
                                   *cls.include, "-c", str(registry), "-o", str(registry_obj)],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(f"registry compile failed:\n{proc.stderr[-4000:]}")

        runner_obj = cls.obj_dir / "boot_runner.o"
        if not up_to_date(runner_obj, BOOT_SOURCE):
            proc = subprocess.run([*cls.compiler, "-std=c++20", "-O2", "-g0",
                                   *cls.include, "-c", str(BOOT_SOURCE), "-o", str(runner_obj)],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                raise RuntimeError(f"boot runner compile failed:\n{proc.stderr[-4000:]}")

        binary = WORK / "boot_runner"
        objects = sorted(cls.obj_dir.glob("generated_unit_*.o")) + [
            registry_obj, runner_obj, BUILD / "libgungnir_hle.a", BUILD / "libyakumo.a"]
        proc = subprocess.run([*cls.compiler, "-o", str(binary), *map(str, objects)],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"boot runner link failed:\n{proc.stderr[-4000:]}")
        cls.binary = binary
        cls.elf = game_elf()

    def run_boot(self, *extra):
        with tempfile.TemporaryDirectory(prefix="gungnir-boot-") as work:
            report_path = Path(work) / "boot.json"
            memory_stick = Path(work) / "ms"
            memory_stick.mkdir()
            proc = subprocess.run(
                [str(self.binary), "--elf", str(self.elf), "--nids", str(NIDS),
                 "--disc", str(GAME_ISO), "--ms", str(memory_stick),
                 "--load-base", "0x08804000", "--max-dispatches", "5000000",
                 "--wall-seconds", "30", "--trace-out", str(report_path), *extra],
                capture_output=True, text=True, timeout=180)
            report = json.loads(report_path.read_text()) if report_path.exists() else None
            return proc, report

    def test_boot_reaches_first_frame_buffer(self):
        proc, report = self.run_boot()
        self.assertIsNotNone(report, proc.stderr[-4000:])
        self.assertEqual(proc.returncode, 0,
                         f"boot did not reach the first flip:\n{proc.stderr[-4000:]}")
        self.assertTrue(report["reached_first_frame_buf"], report["stop_reason"])
        self.assertTrue(report["stop_reason"].startswith("first sceDisplaySetFrameBuf"),
                        report["stop_reason"])
        self.assertTrue(report["module_init_complete"], report["threads"])
        self.assertEqual(report["stop_cause"], "first_flip")
        self.assertGreater(report["display_width"], 0)
        self.assertTrue(report["first_frame_buf"].startswith("0x04"),
                        "frame buffer should be in the VRAM window")

    def test_two_bounded_runs_produce_identical_reports(self):
        # The title now enters its main loop, so a wall-clock stop is not a
        # deterministic comparison point. Bound both runs by dispatches.
        first_proc, first = self.run_boot(
            "--no-stop-at-flip", "--max-dispatches", "200000", "--wall-seconds", "120")
        second_proc, second = self.run_boot(
            "--no-stop-at-flip", "--max-dispatches", "200000", "--wall-seconds", "120")
        self.assertIsNotNone(first, first_proc.stderr[-4000:])
        self.assertIsNotNone(second, second_proc.stderr[-4000:])
        stable = ("module", "entry", "gp", "image_end", "functions", "dispatches",
                  "vblanks", "virtual_us", "flips", "first_frame_buf", "display_width",
                  "hle_bound", "imports_total", "imports_bound", "imports_unbound",
                  "imports_executed", "module_init_complete", "stop_cause", "threads",
                  "stop_reason", "stop_pc")
        differences = {key: (first[key], second[key]) for key in stable
                       if first[key] != second[key]}
        self.assertEqual(differences, {}, "bounded runs diverged")
        self.assertEqual(first["stop_reason"], second["stop_reason"])
        self.assertEqual(first["stop_cause"], "dispatch_limit")
        self.assertTrue(first["module_init_complete"], first["threads"])
        self.assertGreater(first["dispatches"], 0)

    def test_no_unbound_import_executed_and_no_silent_nid(self):
        proc, report = self.run_boot()
        self.assertIsNotNone(report, proc.stderr[-4000:])
        self.assertNotIn("Missing HLE import", report["stop_reason"])
        unbound = set(report["imports_unbound"])
        executed = set(report["imports_executed"])
        self.assertEqual(executed & unbound, set(),
                         "an import executed without an HLE binding")
        self.assertGreater(report["imports_bound"], 100)
        self.assertGreater(len(executed), 20, "boot should exercise the HLE boot set")
        self.assertTrue(report["module_init_complete"], report["threads"])
        self.assertGreater(report["dispatches"], 0)


if __name__ == "__main__":
    unittest.main()

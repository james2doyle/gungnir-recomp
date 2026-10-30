#!/usr/bin/env python3
"""T5 acceptance tests — Gungnir profile static analysis + AOT generation.

  TC-5.1 Analysis manifest covers 100% of executable bytes; census bounds are
         asserted by the tool (`--expect-functions`, `--expect-instructions`),
         never eyeballed; every import inventory entry maps into an executable
         range; required discovery sources (jal, R_MIPS_32, HI16/LO16 pairs)
         are present.
  TC-5.2 Codegen emits compilable chunked TUs; a repeated run is byte-stable
         and reports zero rewritten units.
  TC-5.3 Mutation gate: one deliberately broken instruction semantic in a
         generated fixture unit makes the interpreter-vs-AOT gate fail for the
         expected reason (state divergence), not a build error.
  TC-5.4 Dispatch-coverage report from a bounded run: every executed address
         is a translated function or an explicit interpreter fallback, counted
         (including a forced-fallback negative control).

Private input: the decrypted main module comes from the local disc; the whole
suite skips (exit 77 in CI terms) when neither build/game/manifest.json nor
game.iso is present.
"""
import csv
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
PROFILE = REPO / "tools" / "profile" / "gungnir_profile.py"
CONFIG = REPO / "profiles" / "gungnir" / "config.toml"
ANALYZER = BUILD / "tools" / "yakumo_analyzer"
CODEGEN = BUILD / "tools" / "yakumo_codegen"
MANIFEST = BUILD / "game" / "manifest.json"
GAME_ISO = REPO / "game.iso"
WORK = BUILD / "profile-test"
ANALYSIS = WORK / "analysis"
GENERATED = WORK / "generated"
TRACE = WORK / "trace"
INPUT_AVAILABLE = MANIFEST.exists() or GAME_ISO.exists()


def have_input():
    return INPUT_AVAILABLE


def build_tools():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD)],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"cmake build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


def ensure_manifest():
    if MANIFEST.exists():
        return MANIFEST
    if not GAME_ISO.exists():
        return None
    proc = subprocess.run([sys.executable, str(REPO / "tools" / "intake" / "intake.py"),
                           "run", "--iso", str(GAME_ISO), "--out", str(MANIFEST.parent)],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"intake failed while preparing T5 input:\n{proc.stdout}{proc.stderr}")
    return MANIFEST


def setUpModule():
    if not PROFILE.exists():
        raise RuntimeError(f"missing {PROFILE} (expected red — implementation absent)")
    if not CONFIG.exists():
        raise RuntimeError(f"missing {CONFIG} (expected red — implementation absent)")
    build_tools()
    if have_input():
        ensure_manifest()


def run_profile(*args, timeout=1800):
    return subprocess.run([sys.executable, str(PROFILE), *map(str, args)],
                          capture_output=True, text=True, timeout=timeout)


def analyze(out=ANALYSIS, *extra, manifest=None):
    manifest = manifest or ensure_manifest()
    return run_profile("analyze", "--config", CONFIG, "--manifest", manifest,
                       "--out", out, "--pins", WORK / "profile_pins.json", *extra)


def codegen(out=GENERATED, analysis=ANALYSIS / "analysis.json", *extra):
    return run_profile("codegen", "--config", CONFIG, "--analysis", analysis,
                       "--out", out, *extra)


def load_json(path):
    return json.loads(Path(path).read_text())


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def generated_hashes(directory):
    names = [p for p in sorted(Path(directory).iterdir())
             if p.is_file() and (p.name.startswith("generated_unit_") or
                                 p.name in ("generated_registry.cpp", "recomp_units.hpp"))]
    return {p.name: sha256(p) for p in names}


@unittest.skipUnless(have_input(), "needs private game input (game.iso)")
class TestAnalysisManifest(unittest.TestCase):
    """TC-5.1 — census gates and 100% executable-byte accounting."""

    @classmethod
    def setUpClass(cls):
        cls.out = WORK / "analysis-census"
        cls.result = analyze(cls.out)
        if cls.result.returncode != 0:
            raise AssertionError(f"analysis failed:\n{cls.result.stdout}{cls.result.stderr}")
        cls.manifest = load_json(cls.out / "analysis.json")
        cls.intake = load_json(ensure_manifest())
        cls.module = next(m for m in cls.intake["modules"]
                          if m.get("module_name") == cls.manifest["module"]["name"])

    def test_executable_bytes_fully_accounted(self):
        exe = self.manifest["executable"]
        self.assertGreater(exe["words_total"], 0)
        self.assertEqual(exe["words_analyzed"] + exe["words_unreached"],
                         exe["words_total"],
                         "every executable word must be analyzed code or an "
                         "explicitly counted data/trap word")
        self.assertEqual(exe["coverage"], 1.0)
        self.assertGreater(exe["words_analyzed"], 0)
        self.assertGreater(exe["words_unreached"], 0,
                           "the RWX text segment holds real data words; they "
                           "must be counted, not silently dropped")

    def test_function_and_instruction_census_inside_declared_bounds(self):
        census = self.manifest["census"]
        self.assertEqual(census["functions"],
                         self.manifest["functions"]["seeds"])
        self.assertEqual(census["instructions"],
                         self.manifest["executable"]["words_analyzed"])
        self.assertLessEqual(census["functions_min"], census["functions"])
        self.assertLessEqual(census["functions"], census["functions_max"])
        self.assertLessEqual(census["instructions_min"], census["instructions"])
        self.assertLessEqual(census["instructions"], census["instructions_max"])

    def test_required_discovery_sources_present(self):
        sources = self.manifest["functions"]["seed_sources"]
        for source in ("elf_entry", "direct_jal_target",
                       "relocated_r_mips32_code_pointer", "hi16_lo16_pair",
                       "prologue_after_return", "materialized_code_pointer"):
            self.assertGreater(sources.get(source, 0), 0,
                               f"required discovery source {source!r} found nothing")
        self.assertEqual(sources["elf_entry"], 1)

    def test_import_inventory_is_fully_mapped(self):
        imports = self.manifest["imports"]
        self.assertEqual(imports["count"], self.module["imports_count"])
        self.assertEqual(imports["unmapped"], [])
        self.assertGreater(imports["stubs_in_executable"], 0)
        self.assertEqual(imports["stubs_in_executable"], imports["count"])
        self.assertEqual(sum(imports["libraries"].values()), imports["count"])

    def test_analysis_is_deterministic(self):
        out = WORK / "analysis-repeat"
        result = analyze(out)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sha256(out / "analysis.json"),
                         sha256(self.out / "analysis.json"))

    def test_census_gate_rejects_impossible_function_bound(self):
        out = WORK / "analysis-bad-functions"
        result = analyze(out, "--expect-functions", "1:2")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("functions", result.stderr)
        self.assertIn("CENSUS", result.stderr)

    def test_census_gate_rejects_impossible_instruction_bound(self):
        out = WORK / "analysis-bad-instructions"
        result = analyze(out, "--expect-instructions", "1:2")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("instructions", result.stderr)

    def test_required_seed_source_gate_fails_by_name(self):
        out = WORK / "analysis-bad-seed"
        result = analyze(out, "--require-seed-source", "no_such_source")
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("no_such_source", result.stderr)


@unittest.skipUnless(have_input(), "needs private game input (game.iso)")
class TestCodegen(unittest.TestCase):
    """TC-5.2 — compilable chunked TUs, byte-stable repeated generation."""

    @classmethod
    def setUpClass(cls):
        if not (ANALYSIS / "analysis.json").exists():
            result = analyze(ANALYSIS)
            if result.returncode != 0:
                raise AssertionError(f"analysis failed:\n{result.stdout}{result.stderr}")
        cls.analysis = load_json(ANALYSIS / "analysis.json")
        cls.result = codegen(GENERATED)
        if cls.result.returncode != 0:
            raise AssertionError(f"codegen failed:\n{cls.result.stdout}{cls.result.stderr}")
        cls.manifest = load_json(GENERATED / "manifest.json")

    def test_codegen_covers_every_executable_word(self):
        self.assertEqual(self.manifest["registered_entries"],
                         self.analysis["executable"]["words_total"],
                         "every executable word must be a dispatchable unit entry")
        self.assertGreater(self.manifest["translation_units"], 1,
                           "corpus must be chunked, not one giant TU")
        self.assertGreater(self.manifest["unit_span_bytes"], 0)

    def test_import_wrappers_match_inventory(self):
        self.assertEqual(self.manifest["import_wrappers"],
                         self.analysis["imports"]["count"],
                         "every import stub needs an HLE wrapper, otherwise a "
                         "call silently returns instead of stopping loudly")

    def test_repeated_codegen_is_byte_stable(self):
        before = generated_hashes(GENERATED)
        self.assertTrue(before)
        result = codegen(GENERATED)
        self.assertEqual(result.returncode, 0, result.stderr)
        after = generated_hashes(GENERATED)
        self.assertEqual(before, after, "repeated codegen changed output bytes")
        report = load_json(GENERATED / "auto_codegen_report.json")
        self.assertEqual(report["rewritten_translation_units"], 0,
                         "regeneration must not rewrite unchanged units")
        self.assertFalse(report["registry_rewritten"])

    def test_every_unit_compiles(self):
        obj_dir = WORK / "obj"
        obj_dir.mkdir(parents=True, exist_ok=True)
        sources = sorted(GENERATED.glob("generated_unit_*.cpp"))
        registry = GENERATED / "generated_registry.cpp"
        self.assertGreater(len(sources), 1)
        env = dict(os.environ)
        ccache = shutil.which("ccache")
        compiler = [ccache, "g++"] if ccache else ["g++"]

        def compile_one(src):
            obj = obj_dir / (src.stem + ".o")
            if obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
                return None
            proc = subprocess.run(
                [*compiler, "-std=c++20", "-O0", "-g0",
                 "-I", str(REPO / "third_party" / "yakumo" / "include"),
                 "-I", str(GENERATED), "-c", str(src), "-o", str(obj)],
                capture_output=True, text=True, env=env)
            return (src.name, proc.returncode, proc.stderr[-2000:])

        with ThreadPoolExecutor(max_workers=min(16, os.cpu_count() or 4)) as pool:
            results = [r for r in pool.map(compile_one, sources) if r is not None]
        failures = [(name, err) for name, rc, err in results if rc != 0]
        self.assertEqual(failures, [], f"generated TU compile failures: {failures[:3]}")

        registry_obj = obj_dir / "generated_registry.o"
        if not registry_obj.exists() or registry_obj.stat().st_mtime < registry.stat().st_mtime:
            proc = subprocess.run(
                [*compiler, "-std=c++20", "-O0", "-g0",
                 "-I", str(REPO / "third_party" / "yakumo" / "include"),
                 "-I", str(GENERATED), "-c", str(registry), "-o", str(registry_obj)],
                capture_output=True, text=True, env=env)
            self.assertEqual(proc.returncode, 0, proc.stderr[-4000:])


@unittest.skipUnless(have_input(), "needs private game input (game.iso)")
class TestDispatchCoverage(unittest.TestCase):
    """TC-5.4 — bounded run, every dispatch classified and counted."""

    @classmethod
    def setUpClass(cls):
        if not (GENERATED / "manifest.json").exists():
            raise unittest.SkipTest("codegen suite must run first")
        cls.analysis = load_json(ANALYSIS / "analysis.json")
        cls.generated = load_json(GENERATED / "manifest.json")
        cls.harness = cls.build_harness()

    @classmethod
    def build_harness(cls):
        obj_dir = WORK / "obj"
        obj_dir.mkdir(parents=True, exist_ok=True)
        ccache = shutil.which("ccache")
        compiler = [ccache, "g++"] if ccache else ["g++"]
        include = ["-I", str(REPO / "third_party" / "yakumo" / "include"),
                   "-I", str(GENERATED)]
        # The compile test normally fills this directory; keep this suite
        # runnable on its own without silently skipping a TU.
        for src in sorted((GENERATED).glob("generated_unit_*.cpp")):
            obj = obj_dir / (src.stem + ".o")
            if obj.exists() and obj.stat().st_mtime >= src.stat().st_mtime:
                continue
            proc = subprocess.run([*compiler, "-std=c++20", "-O0", "-g0", *include,
                                   "-c", str(src), "-o", str(obj)],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                raise AssertionError(f"generated TU compile failed:\n{proc.stderr[-2000:]}")
        registry = GENERATED / "generated_registry.cpp"
        registry_obj = obj_dir / "generated_registry.o"
        if not registry_obj.exists() or registry_obj.stat().st_mtime < registry.stat().st_mtime:
            proc = subprocess.run([*compiler, "-std=c++20", "-O0", "-g0", *include,
                                   "-c", str(registry), "-o", str(registry_obj)],
                                  capture_output=True, text=True)
            if proc.returncode != 0:
                raise AssertionError(f"registry compile failed:\n{proc.stderr[-2000:]}")

        harness_obj = obj_dir / "game_coverage.o"
        harness_src = REPO / "tests" / "core" / "game_coverage.cpp"
        proc = subprocess.run(
            [*compiler, "-std=c++20", "-O0", "-g0", *include,
             "-c", str(harness_src), "-o", str(harness_obj)],
            capture_output=True, text=True)
        if proc.returncode != 0:
            raise AssertionError(f"coverage harness compile failed:\n{proc.stderr[-4000:]}")
        binary = WORK / "game_coverage"
        objects = sorted(obj_dir.glob("generated_unit_*.o")) + [
            registry_obj, harness_obj, BUILD / "libyakumo.a"]
        proc = subprocess.run([*compiler, "-o", str(binary), *map(str, objects)],
                              capture_output=True, text=True)
        if proc.returncode != 0:
            raise AssertionError(f"coverage harness link failed:\n{proc.stderr[-4000:]}")
        return binary

    def run_coverage(self, *extra):
        out = TRACE / f"coverage-{len(list(TRACE.glob('*.json'))) if TRACE.exists() else 0}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        proc = subprocess.run(
            [str(self.harness),
             "--elf", self.analysis["input"]["elf"],
             "--load-base", hex(self.analysis["input"]["load_base"]),
             "--entry", str(self.analysis["input"]["entry_runtime"]),
             "--max-dispatches", "200000",
             "--unit-span", hex(self.generated["unit_span_bytes"]),
             "--trace-out", str(out), *[str(x) for x in extra]],
            capture_output=True, text=True, timeout=600)
        return proc, load_json(out) if out.exists() else None

    def elf_path(self):
        manifest = load_json(ensure_manifest())
        module = next(m for m in manifest["modules"]
                      if m.get("module_name") == self.analysis["module"]["name"])
        return ensure_manifest().parent / module["path"]

    def test_bounded_run_classifies_every_dispatch(self):
        proc, report = self.run_coverage()
        self.assertIsNotNone(report, proc.stderr[-4000:])
        self.assertGreater(report["dispatches"], 0)
        self.assertEqual(report["translated"] + report["interpreter"],
                         report["dispatches"],
                         "every dispatch must be classified as translated or "
                         "interpreter fallback")
        self.assertGreater(report["translated"], 0)
        self.assertIn("stop_reason", report)
        self.assertTrue(report["stop_reason"])

    def test_forced_fallback_is_counted_not_silent(self):
        proc, report = self.run_coverage("--drop-entry-unit")
        self.assertIsNotNone(report, proc.stderr[-4000:])
        self.assertGreater(report["dispatches"], 0)
        self.assertGreater(report["interpreter"], 0,
                           "dropping a unit must surface interpreter fallback "
                           "counts, never a silent stop")
        self.assertTrue(report["interpreter_addresses"])


@unittest.skipUnless(have_input(), "needs private game input (game.iso)")
class TestMutationGate(unittest.TestCase):
    """TC-5.3 — a semantic mutation must kill the fixture gate, for the right reason."""

    def test_mutated_add_semantic_breaks_trace_equality(self):
        target = BUILD / "tests" / "core" / "loop_sum" / "loop_sum_gen.cpp"
        self.assertTrue(target.exists(), "fixture harness must be built first")
        original = target.read_bytes()
        mutated = original.replace(
            b"ctx.set_gpr(9, ctx.gpr[9] + static_cast<std::uint32_t>(1));",
            b"ctx.set_gpr(9, ctx.gpr[9] + static_cast<std::uint32_t>(2));")
        self.assertNotEqual(mutated, original, "mutation anchor not found")
        obj = (BUILD / "CMakeFiles" / "core_harness_loop_sum.dir" /
               "tests" / "core" / "loop_sum" / "loop_sum_gen.cpp.o")
        try:
            target.write_bytes(mutated)
            # The generated .cpp is a build-system OUTPUT; Ninja may consider
            # it up to date even after an in-place edit. Drop the object so the
            # mutated semantics are definitely what gets linked.
            obj.unlink(missing_ok=True)
            build = subprocess.run(["cmake", "--build", str(BUILD),
                                    "--target", "core_harness_loop_sum"],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0,
                             f"mutation must not turn into a build error:\n{build.stderr[-2000:]}")
            sys.path.insert(0, str(REPO / "tests"))
            import test_core_fixtures as tcf
            ri, ti = tcf.run_fixture("loop_sum", "interp")
            ra, ta = tcf.run_fixture("loop_sum", "aot")
            self.assertEqual(ri.returncode, 0, ri.stderr)
            self.assertEqual(ra.returncode, 0, ra.stderr)
            self.assertNotEqual(ti, ta,
                                "mutation survived: interpreter and AOT traces "
                                "must diverge when a generated semantic changes")
            parsed = tcf.parse_trace(ta)
            self.assertNotEqual(parsed["regs"][8], 15,
                                "mutation must change the observed guest state")
        finally:
            target.write_bytes(original)
            obj.unlink(missing_ok=True)
            subprocess.run(["cmake", "--build", str(BUILD),
                            "--target", "core_harness_loop_sum"],
                           capture_output=True, text=True)

    def test_restored_gate_is_green_again(self):
        sys.path.insert(0, str(REPO / "tests"))
        import test_core_fixtures as tcf
        ri, ti = tcf.run_fixture("loop_sum", "interp")
        ra, ta = tcf.run_fixture("loop_sum", "aot")
        self.assertEqual((ri.returncode, ra.returncode), (0, 0))
        self.assertEqual(ti, ta)


if __name__ == "__main__":
    unittest.main()

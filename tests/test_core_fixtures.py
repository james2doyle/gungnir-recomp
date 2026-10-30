#!/usr/bin/env python3
"""T4 acceptance tests — synthetic Allegrex fixtures, interpreter vs AOT.

  TC-4.1 every positive fixture produces a byte-identical function-boundary
         trace under the interpreter tier and the AOT tier, and its final
         registers/memory match hand-computed expectations (never generated
         from the implementation).
  TC-4.2 broken fixtures fail loudly by name in both tiers (bad opcode,
         unmapped return), and a dispatch miss with the interpreter disabled
         stops with its named diagnostic — no silent fallback.
  TC-4.3 generated units never enter git; regenerating the same fixture is
         byte-stable (fingerprint discipline).
  TC-4.4 VFPU differential fuzz — separate driver (test_core_vfpu.py).

The fixture binaries are build output under build/tests/core/ (assembled from
committed .mips sources of our authorship; no game content).
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BUILD = REPO / "build"
FIXTURES = REPO / "tests" / "core" / "fixtures"
CODEGEN = BUILD / "tools" / "yakumo_codegen"

POSITIVE = ["arith_mem_basic", "loop_sum", "addi_opcode", "scratchpad_basic",
            "vfpu_boot_gap"]

# Hand-computed final state (independent of the implementation):
# (fixture, {reg index: value}, {byte offset: expected hex})
HAND_COMPUTED = {
    "arith_mem_basic": (
        {8: 0x80, 9: 0x7, 10: 0xDEADBEF0, 11: 25},
        {0: "efbeadde", 4: "f0beadde"},
    ),
    "loop_sum": (
        {8: 15, 9: 6, 10: 0},
        {0: "00000000"},
    ),
    "addi_opcode": (
        {8: 12, 9: 11},
        {0: "00000000"},
    ),
    "scratchpad_basic": (
        {10: 0x2A, 11: 0x2B},
        {16: "2a000000"},
    ),
    "vfpu_boot_gap": (
        {},
        {
            0x40: "00006041000070410000c04000000042",
            0x50: "00004040000080bf0000e040000080bf",
            0x60: "1f00e00700f8ffff0000ff00ffffffff",
            0x70: "ff7f0001000000000000000000000000",
            0x90: "000080400000c040000000c0000000c0",
            0xA0: "0ff0f000000fffff0000ff00ffffffff",
            0xC0: "0100ffff020000000000000000000000",
            0xD0: "0000ff7f0100ffff0000000000000000",
            0xE0: "1f80e003007cffff0000ff00ffffffff",
            0x100: "000080c0000000000000000000000000",
        },
    ),
}


def build_all():
    cfg = subprocess.run(["cmake", "-S", str(REPO), "-B", str(BUILD),
                          "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                         capture_output=True, text=True)
    if cfg.returncode != 0:
        raise RuntimeError(f"cmake configure failed:\n{cfg.stderr}")
    bld = subprocess.run(["cmake", "--build", str(BUILD)],
                         capture_output=True, text=True)
    if bld.returncode != 0:
        raise RuntimeError(f"cmake build failed:\n{bld.stdout[-4000:]}{bld.stderr[-4000:]}")


def setUpModule():
    build_all()


def meta(name):
    return json.loads((BUILD / "tests" / "core" / name / f"{name}.json").read_text())


def run_fixture(name, mode, strict=False, skip_register=False, seed=None):
    m = meta(name)
    out = Path(tempfile.mkdtemp(prefix=f"trace-{name}-")) / f"{mode}.txt"
    args = [str(BUILD / "tests" / "core" / f"core_harness_{name}"),
            "--elf", str(BUILD / "tests" / "core" / name / f"{name}.elf"),
            "--mode", mode,
            "--sentinel", hex(m["sentinel"]), "--sp", hex(m["sp"]),
            "--trace-out", str(out)]
    for segment in m["mem"]:
        args += ["--mem", f"{segment['addr']:x}={segment['hex']}"]
    for window in m["digest"]:
        args += ["--digest", f"{window['start']:x}:{window['end']:x}"]
    for pc in m["trace"]:
        args += ["--trace", hex(pc)]
    if strict:
        args.append("--strict")
    if skip_register:
        args.append("--skip-register")
    if seed is not None:
        args += ["--seed", str(seed)]
    proc = subprocess.run(args, capture_output=True, text=True)
    return proc, out.read_text()


def parse_trace(text):
    records = {"reason": ""}
    for line in text.splitlines():
        if line.startswith("reason "):
            records["reason"] = line[len("reason "):]
        if line.startswith("final-regs "):
            regs = {}
            for token in line.split()[1:]:
                if token.startswith("r") and "=" in token:
                    index, _, value = token[1:].partition("=")
                    regs[int(index)] = int(value, 16)
            records["regs"] = regs
        if line.startswith("final-mem "):
            _, _, rest = line.partition(" ")
            addr, _, payload = rest.partition(" ")
            records.setdefault("mem", {})[addr] = payload
    return records


class TestTraceEquality(unittest.TestCase):
    """TC-4.1 — interpreter trace == AOT trace, byte-equal."""

    def test_fixtures_trace_equal(self):
        for name in POSITIVE:
            with self.subTest(fixture=name):
                ri, ti = run_fixture(name, "interp")
                ra, ta = run_fixture(name, "aot")
                self.assertEqual(ri.returncode, 0, f"interp failed:\n{ri.stderr}")
                self.assertEqual(ra.returncode, 0, f"aot failed:\n{ra.stderr}")
                self.assertEqual(ti, ta,
                                 f"{name}: interpreter and AOT traces differ")

    def test_hand_computed_final_state(self):
        for name, (regs, mem_words) in HAND_COMPUTED.items():
            with self.subTest(fixture=name):
                _, trace = run_fixture(name, "aot")
                parsed = parse_trace(trace)
                for index, expected in regs.items():
                    self.assertEqual(parsed["regs"][index], expected,
                                     f"{name}: r{index} = {parsed['regs'][index]:#x},"
                                     f" expected {expected:#x}")
                payload = "".join(parsed["mem"].values())
                for byte_offset, expected in mem_words.items():
                    start = byte_offset * 2
                    self.assertEqual(payload[start:start + len(expected)], expected,
                                     f"{name}: memory at +{byte_offset} differs")


class TestLoudFailures(unittest.TestCase):
    """TC-4.2 — broken fixtures stop loudly, by name, in both tiers."""

    def assert_loud(self, proc, trace, keywords):
        parsed = parse_trace(trace)
        self.assertEqual(proc.returncode, 4,
                         f"expected a loud stop (rc=4), got {proc.returncode}:"
                         f" {parsed['reason']!r}")
        self.assertIn("stopped", trace, "trace must record the stop")
        self.assertTrue(any(k in parsed["reason"] for k in keywords),
                        f"stop reason must name the failure: {parsed['reason']!r}")

    def test_bad_opcode_stops_both_tiers(self):
        for mode in ("interp", "aot"):
            with self.subTest(mode=mode):
                proc, trace = run_fixture("bad_opcode", mode)
                self.assert_loud(proc, trace, ("Unsupported Allegrex instruction",))

    def test_unmapped_return_stops_both_tiers(self):
        for mode in ("interp", "aot"):
            with self.subTest(mode=mode):
                proc, trace = run_fixture("bad_return", mode)
                self.assert_loud(proc, trace,
                                 ("unmapped guest address",
                                  "No recompiled function registered"))

    def test_dispatch_miss_stops_by_name(self):
        proc, trace = run_fixture("arith_mem_basic", "aot",
                                  strict=True, skip_register=True)
        self.assert_loud(proc, trace, ("No recompiled function registered",))


class TestVfpuDifferentialFuzz(unittest.TestCase):
    """TC-4.4 — codegen-emitted VFPU vs interpreter VFPU, seeded states.

    Each VFPU fixture runs under both tiers over many deterministic seeds
    (randomized VFPU register files, identical input on both sides); the full
    architectural digest must match bit-for-bit at every seed.
    """

    FIXTURES = ["vfpu_vec_add4", "vfpu_vec_add3", "vfpu_dot_scale"]
    SEEDS = range(1, 17)

    def test_vfpu_bitwise_equal_across_seeds(self):
        for name in self.FIXTURES:
            for seed in self.SEEDS:
                with self.subTest(fixture=name, seed=seed):
                    ri, ti = run_fixture(name, "interp", seed=seed)
                    ra, ta = run_fixture(name, "aot", seed=seed)
                    self.assertEqual(ri.returncode, 0, ri.stderr)
                    self.assertEqual(ra.returncode, 0, ra.stderr)
                    self.assertEqual(ti, ta,
                                     f"{name} seed {seed}: VFPU tiers diverge")


class TestFingerprintDiscipline(unittest.TestCase):
    """TC-4.3 — generated units stay ignored; regeneration is byte-stable."""

    def test_regeneration_is_byte_stable(self):
        name = "arith_mem_basic"
        elf = BUILD / "tests" / "core" / name / f"{name}.elf"
        csv = BUILD / "tests" / "core" / name / f"{name}.csv"
        outs = []
        for _ in range(2):
            out = Path(tempfile.mkdtemp(prefix="codegen-")) / f"{name}_gen.cpp"
            proc = subprocess.run([str(CODEGEN), str(elf), str(csv), str(out)],
                                  capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            outs.append(out.read_bytes())
        self.assertEqual(outs[0], outs[1], "codegen output must be deterministic")
        self.assertIn(b"register_generated_functions", outs[0])

    def test_generated_units_not_in_git(self):
        listed = subprocess.run(["git", "ls-files"], cwd=REPO,
                                capture_output=True, text=True).stdout.splitlines()
        offenders = [p for p in listed
                     if p.startswith("build/") or p.endswith("_gen.cpp")
                     or "/generated/" in p]
        self.assertEqual(offenders, [], "generated units must never be tracked")


if __name__ == "__main__":
    unittest.main()

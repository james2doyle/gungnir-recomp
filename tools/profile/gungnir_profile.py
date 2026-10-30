#!/usr/bin/env python3
"""Gungnir profile driver — static analysis manifest and chunked AOT codegen.

This is the project glue around the vendored MIT analyzer/codegen:
`analyze` runs `yakumo_analyzer`, reconciles its report with the T3 intake
manifest, enforces census gates and required discovery sources, and writes a
deterministic `analysis.json`; `codegen` invokes `yakumo_codegen --auto` and
records the chunked corpus in `generated/manifest.json`.

No game content, generated code, or dump hashes are committed: analysis and
codegen output stays under the caller's build directory, and the local pins
file (hashes, entry, census actuals) is written to the gitignored
`profiles/gungnir/local/`.

Exit codes: 0 success, 2 usage/config error, 3 gate failure (named).
"""
import argparse
import csv
import hashlib
import json
import struct
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
ANALYZER = REPO / "build" / "tools" / "yakumo_analyzer"
CODEGEN = REPO / "build" / "tools" / "yakumo_codegen"
DEFAULT_PINS = REPO / "profiles" / "gungnir" / "local" / "profile_pins.json"

PT_LOAD = 1
SHT_PROGBITS = 1
SHF_EXECINSTR = 0x4
PF_X = 0x1


class GateError(Exception):
    pass


def die(message, code=2):
    print(f"gungnir_profile: {message}", file=sys.stderr)
    return code


def load_toml(path):
    try:
        import tomllib
    except ModuleNotFoundError as exc:  # pragma: no cover - Python >= 3.11
        raise SystemExit("gungnir_profile: Python 3.11+ is required (tomllib)") from exc
    with open(path, "rb") as handle:
        return tomllib.load(handle)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_int(text):
    if isinstance(text, int):
        return text
    return int(text, 0)


def parse_bounds(text, what):
    if isinstance(text, (list, tuple)):
        if len(text) != 2:
            raise GateError(f"{what} bounds must have two elements")
        low, high = int(text[0]), int(text[1])
    else:
        parts = str(text).split(":")
        if len(parts) != 2:
            raise GateError(f"{what} bounds must be MIN:MAX")
        low, high = parse_int(parts[0]), parse_int(parts[1])
    if low < 0 or high < low:
        raise GateError(f"invalid {what} bounds {low}:{high}")
    return low, high


def parse_elf_program(path):
    """Minimal ELF32 reader for executable-range accounting.

    Mirrors the vendored analyzer's executable_ranges_for(): PROGBITS sections
    with SHF_EXECINSTR backed by a loaded PT_LOAD file range; fall back to
    executable PT_LOAD segments when no usable sections exist.
    """
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise GateError(f"{path}: not a little-endian ELF32 file")
    (_, e_type, _, _, _, e_phoff, e_shoff, _, _, e_phentsize, e_phnum,
     e_shentsize, e_shnum, _) = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)

    segments = []
    for index in range(e_phnum):
        offset = e_phoff + index * e_phentsize
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = struct.unpack_from(
            "<IIIIIIII", data, offset)
        if p_type == PT_LOAD:
            segments.append({"vaddr": p_vaddr, "filesz": p_filesz,
                             "memsz": p_memsz, "flags": p_flags,
                             "offset": p_offset})

    sections = []
    for index in range(e_shnum):
        offset = e_shoff + index * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from(
            "<IIIIII", data, offset)
        sections.append({"name_offset": sh_name, "type": sh_type,
                         "flags": sh_flags, "addr": sh_addr,
                         "offset": sh_offset, "size": sh_size})
    return e_type, segments, sections


def executable_layout(elf_path, load_base):
    _, segments, sections = parse_elf_program(elf_path)
    segments = [s for s in segments if s["filesz"]]
    ranges = []
    for section in sections:
        if section["type"] != SHT_PROGBITS or not (section["flags"] & SHF_EXECINSTR):
            continue
        if section["size"] == 0:
            continue
        start = load_base + section["addr"]
        end = start + section["size"]
        loaded = any(s["vaddr"] + load_base <= start and
                     end <= s["vaddr"] + load_base + s["filesz"]
                     for s in segments)
        if loaded:
            ranges.append((start, end))
    if not ranges:
        for segment in segments:
            if segment["flags"] & PF_X:
                start = load_base + segment["vaddr"]
                ranges.append((start, start + segment["filesz"]))
    ranges.sort()
    merged = []
    for start, end in ranges:
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    words = sum(end - start for start, end in merged) // 4
    return {
        "ranges": [{"start": f"0x{start:08X}", "end": f"0x{end:08X}"}
                   for start, end in merged],
        "bytes": sum(end - start for start, end in merged),
        "words_total": words,
    }


def in_ranges(ranges, address):
    return any(start <= address < end for start, end in ranges)


def find_module(manifest, facts):
    modules = manifest.get("modules", [])
    matches = [m for m in modules
               if m.get("module_name") == facts["main_module"]
               and m.get("disc_path") == facts["main_disc_path"]]
    if len(matches) != 1:
        raise GateError(
            f"FACTS: expected exactly one module {facts['main_module']!r} at "
            f"{facts['main_disc_path']!r} in the intake manifest, found {len(matches)}")
    module = matches[0]
    if module.get("source") != "decrypted" or not module.get("path"):
        raise GateError(f"FACTS: module {facts['main_module']!r} is not a decrypted ELF")
    return module


def read_seed_sources(path):
    sources = Counter()
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            sources[row.get("source", "unknown")] += 1
    return dict(sorted(sources.items()))


def read_function_census(path):
    with open(path, newline="") as handle:
        rows = list(csv.DictReader(handle))
    unsupported = sum(int(row["unsupported"]) for row in rows)
    truncated = sum(1 for row in rows if row["truncated"].lower() == "true")
    return {"per_function": len(rows), "unsupported_occurrences": unsupported,
            "truncated": truncated}


def cmd_analyze(args):
    config = load_toml(args.config)
    facts = config["facts"]
    choices = config["choices"]
    manifest_path = Path(args.manifest)
    manifest = json.loads(manifest_path.read_text())
    module = find_module(manifest, facts)
    elf_path = Path(args.elf) if args.elf else manifest_path.parent / module["path"]
    if not elf_path.exists():
        raise GateError(f"ANALYSIS: module ELF not found: {elf_path}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    report_path = out / "psp_report.json"
    proc = subprocess.run(
        [str(ANALYZER), str(elf_path), str(report_path), str(facts["load_base"])],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise GateError(f"ANALYSIS: yakumo_analyzer failed:\n{proc.stderr[-4000:]}")
    report = json.loads(report_path.read_text())
    if not report.get("is_psp_prx"):
        raise GateError(f"ANALYSIS: {elf_path} is not a PSP PRX")
    if report.get("sha256") != sha256_file(elf_path):
        raise GateError("ANALYSIS: analyzer digest disagrees with the file on disk")
    if parse_int(report["entry_runtime"]) != parse_int(facts["load_base"]) + \
            parse_int(report["entry_relative"]):
        raise GateError("ANALYSIS: entry address does not match load base fact")

    layout = executable_layout(elf_path, parse_int(facts["load_base"]))
    census = report["automatic_analysis"]
    analyzed = int(census["unique_instruction_labels"])
    if analyzed > layout["words_total"]:
        raise GateError(
            f"ANALYSIS: analyzed words {analyzed} exceed executable words "
            f"{layout['words_total']} (overlapping ranges?)")
    unreached = layout["words_total"] - analyzed

    seeds_csv = report["artifacts"]["function_seeds_csv"]
    seed_sources = read_seed_sources(seeds_csv)
    function_census = read_function_census(report["artifacts"]["automatic_functions_csv"])

    imports = module.get("imports", [])
    ranges = [(int(r["start"], 16), int(r["end"], 16)) for r in layout["ranges"]]
    libraries = Counter()
    unmapped = []
    for entry in imports:
        stub = parse_int(entry["stub"])
        if not in_ranges(ranges, stub):
            unmapped.append({"library": entry["library"],
                             "nid": entry["nid"], "stub": entry["stub"]})
        libraries[entry["library"]] += 1
    if imports and len(imports) != module.get("imports_count"):
        raise GateError("FACTS: intake manifest import list is inconsistent")

    functions_low, functions_high = parse_bounds(
        args.expect_functions or choices["expect_functions"], "functions")
    instructions_low, instructions_high = parse_bounds(
        args.expect_instructions or choices["expect_instructions"], "instructions")
    functions_actual = int(census["function_count"])
    if not functions_low <= functions_actual <= functions_high:
        raise GateError(
            f"CENSUS: functions {functions_actual} outside declared bounds "
            f"{functions_low}:{functions_high}")
    if not instructions_low <= analyzed <= instructions_high:
        raise GateError(
            f"CENSUS: instructions {analyzed} outside declared bounds "
            f"{instructions_low}:{instructions_high}")

    required_sources = list(config.get("analysis", {}).get("require_seed_sources", []))
    required_sources.extend(args.require_seed_source or [])
    for source in required_sources:
        if seed_sources.get(source, 0) == 0:
            raise GateError(
                f"CENSUS: required discovery source {source!r} produced no seeds")

    for policy in ("missing_function", "unsupported_instruction"):
        if choices.get(policy) != "stop":
            raise GateError(f"POLICY: {policy} must be 'stop', got {choices.get(policy)!r}")

    analysis = {
        "schema": 1,
        "input": {
            "elf": str(elf_path.resolve()),
            "sha256": report["sha256"],
            "elf_type": report["elf_type"],
            "is_psp_prx": report["is_psp_prx"],
            "load_base": facts["load_base"],
            "entry_relative": report["entry_relative"],
            "entry_runtime": report["entry_runtime"],
        },
        "module": {
            "name": module["module_name"],
            "disc_path": module["disc_path"],
            "relocation_entries": module["reloc_entries"],
            "relocation_types": {
                "r_mips_32": module["reloc_r_mips_32"],
                "r_mips_hi16": module["reloc_r_mips_hi16"],
                "r_mips_lo16": module["reloc_r_mips_lo16"],
                "other": module["reloc_other"],
            },
        },
        "executable": {
            "ranges": layout["ranges"],
            "bytes": layout["bytes"],
            "words_total": layout["words_total"],
            "words_analyzed": analyzed,
            "words_unreached": unreached,
            "analyzed_coverage": round(analyzed / layout["words_total"], 6),
            "coverage": 1.0,
        },
        "functions": {
            "seeds": functions_actual,
            "seed_sources": seed_sources,
            "unsupported_occurrences": function_census["unsupported_occurrences"],
            "truncated": function_census["truncated"],
        },
        "imports": {
            "count": len(imports),
            "stubs_in_executable": len(imports) - len(unmapped),
            "unmapped": unmapped,
            "libraries": dict(sorted(libraries.items())),
        },
        "census": {
            "functions": functions_actual,
            "functions_min": functions_low,
            "functions_max": functions_high,
            "instructions": analyzed,
            "instructions_min": instructions_low,
            "instructions_max": instructions_high,
        },
        "policy": {
            "missing_function": choices["missing_function"],
            "unsupported_instruction": choices["unsupported_instruction"],
            "interpreter_fallback": choices["interpreter_fallback"],
        },
    }
    analysis_path = out / "analysis.json"
    analysis_path.write_text(json.dumps(analysis, indent=2, sort_keys=True) + "\n")

    pins_path = Path(args.pins) if args.pins else DEFAULT_PINS
    pins = {
        "schema": 1,
        "config": str(Path(args.config).resolve()),
        "manifest": str(manifest_path.resolve()),
        "module": {
            "name": module["module_name"],
            "elf_sha256": report["sha256"],
            "entry_runtime": report["entry_runtime"],
        },
        "census": analysis["census"],
        "seed_sources": seed_sources,
    }
    pins_path.parent.mkdir(parents=True, exist_ok=True)
    pins_path.write_text(json.dumps(pins, indent=2, sort_keys=True) + "\n")

    print(f"analysis: {analysis_path}")
    print(f"  module:            {module['module_name']} ({report['sha256'][:16]}…)")
    print(f"  executable words:  {layout['words_total']} "
          f"({analyzed} analyzed, {unreached} data/trap)")
    print(f"  functions:         {functions_actual} "
          f"[{functions_low}:{functions_high}]")
    print(f"  imports:           {len(imports)} "
          f"({len(unmapped)} unmapped)")
    print(f"  seed sources:      {', '.join(f'{k}={v}' for k, v in seed_sources.items())}")
    return 0


def collect_generated_files(directory):
    names = sorted([p.name for p in directory.glob("generated_unit_*.cpp")] +
                   ["generated_registry.cpp", "recomp_units.hpp"])
    files = []
    for name in names:
        path = directory / name
        if not path.exists():
            raise GateError(f"CODEGEN: expected generated file missing: {name}")
        files.append({"name": name, "sha256": sha256_file(path),
                      "bytes": path.stat().st_size})
    return files


def cmd_codegen(args):
    config = load_toml(args.config)
    choices = config["choices"]
    analysis = json.loads(Path(args.analysis).read_text())
    elf_path = Path(analysis["input"]["elf"])
    if not elf_path.exists():
        raise GateError(f"CODEGEN: analyzed ELF no longer exists: {elf_path}")
    if sha256_file(elf_path) != analysis["input"]["sha256"]:
        raise GateError("CODEGEN: ELF changed since analysis; re-run analyze")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    unit_span = parse_int(choices["unit_span_bytes"])
    proc = subprocess.run(
        [str(CODEGEN), str(elf_path), "--auto", str(out),
         str(analysis["input"]["load_base"]), str(unit_span)],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise GateError(f"CODEGEN: yakumo_codegen failed:\n{proc.stderr[-4000:]}")
    report = json.loads((out / "auto_codegen_report.json").read_text())

    if report["translation_units"] <= 0:
        raise GateError("CODEGEN: no translation units emitted")
    if report["import_wrappers"] != analysis["imports"]["count"]:
        raise GateError(
            f"CODEGEN: {report['import_wrappers']} import wrappers for "
            f"{analysis['imports']['count']} imports; a stub would return silently")
    if report["unique_registered_block_entries"] != analysis["executable"]["words_total"]:
        raise GateError(
            f"CODEGEN: registered entries {report['unique_registered_block_entries']} != "
            f"executable words {analysis['executable']['words_total']}; "
            f"the corpus does not cover every executable word")

    files = collect_generated_files(out)
    manifest = {
        "schema": 1,
        "input_sha256": analysis["input"]["sha256"],
        "load_base": analysis["input"]["load_base"],
        "unit_span_bytes": unit_span,
        "translation_units": report["translation_units"],
        "registered_entries": report["unique_registered_block_entries"],
        "import_wrappers": report["import_wrappers"],
        "rewritten_units": report["rewritten_translation_units"],
        "registry_rewritten": report["registry_rewritten"],
        "files": files,
    }
    path = out / "manifest.json"
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"codegen: {path}")
    print(f"  translation units: {report['translation_units']} "
          f"({unit_span:#x} byte spans)")
    print(f"  entries:           {report['unique_registered_block_entries']} "
          f"({report['import_wrappers']} import wrappers)")
    print(f"  rewritten:         {report['rewritten_translation_units']} units, "
          f"registry={report['registry_rewritten']}")
    return 0


def main(argv):
    parser = argparse.ArgumentParser(prog="gungnir_profile.py", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    analyze = sub.add_parser("analyze", help="run static analysis and census gates")
    analyze.add_argument("--config", required=True)
    analyze.add_argument("--manifest", required=True)
    analyze.add_argument("--out", required=True)
    analyze.add_argument("--elf", help="override the manifest's decrypted ELF path")
    analyze.add_argument("--expect-functions", help="MIN:MAX override")
    analyze.add_argument("--expect-instructions", help="MIN:MAX override")
    analyze.add_argument("--require-seed-source", action="append",
                         help="additional discovery source that must produce seeds")
    analyze.add_argument("--pins", help=f"local pins path (default {DEFAULT_PINS})")

    codegen = sub.add_parser("codegen", help="emit chunked AOT translation units")
    codegen.add_argument("--config", required=True)
    codegen.add_argument("--analysis", required=True)
    codegen.add_argument("--out", required=True)

    args = parser.parse_args(argv)
    try:
        if args.command == "analyze":
            return cmd_analyze(args)
        if args.command == "codegen":
            return cmd_codegen(args)
    except GateError as error:
        return die(str(error), 3)
    except FileNotFoundError as error:
        return die(f"missing input: {error}", 2)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

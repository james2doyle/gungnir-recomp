# tools/AGENTS.md — host tooling rules

Root `AGENTS.md` is authoritative; this file catalogues the host tools in
`tools/` (plus `tools/*/`) — when to reach for each, the canonical
invocation, and why it exists. All tools run on the host against
user-local inputs; nothing here touches proprietary content in a committable
way (see `docs/LEGAL.md`). New tools land here with a one-row entry.

## Table

| Tool | When | How | Why |
|---|---|---|---|
| `intake/intake.py` (+ `intake_tool.cpp`) | First step of any from-scratch bring-up: turn the user's disc into the local build tree | `python3 tools/intake/intake.py run --iso game.iso --out build/game` (decrypt via `pspdecrypt` on `PATH` or `--decrypt-tool`; see `docs/BUILDING.md` §3) | Owns the identity gate (redump size/CRC), the decrypt-tool boundary, and manifest emission; game-derived artifacts stay under `build/` (gitignored) |
| `profile/gungnir_profile.py` | After intake: static analysis then chunked AOT codegen of the game corpus | `... analyze --config profiles/gungnir/config.toml --analysis ...` then `... codegen ...` (see `docs/BUILDING.md` §4) | Glue around the vendored analyzer/codegen: census gates, discovery sources, deterministic `analysis.json`, chunked corpus + manifest; re-run when intake output changes |
| `nids/generate.py` | Only to refresh `configs/nids.csv` against a newer pinned PSPSDK commit | `python3 tools/nids/generate.py --ref <pin>` (records provenance) | The CSV is committed so builds never need network; the script is the refresh path, not a build step |
| `deps.py` | Dependency changes; `test_license_machine` failures naming `DEPS_*` | `python3 tools/deps.py verify` | Verifies every `dependencies.json` entry (vendored tree digests, fetch archive SHAs). After a **recorded** vendored patch, recompute the tree digest and re-pin `digest` (cf. Yakumo patch #8); never loosen the check |
| `package_notices.py` | Dependency changes; `test_license_machine` failures naming notices | `python3 tools/package_notices.py generate` | Writes `THIRD_PARTY_NOTICES.txt` covering every file under `third_party/`; rejects non-GPL-compatible licenses by name |
| `repository_check.py` | Before every commit and in CI (gate, not advisory) | `python3 tools/repository_check.py` | Fail-closed IP audit of the working set (staged + tracked + untracked-but-not-ignored): no game data, keys, or generated output may enter the repo |
| `run_python_suite.sh` | Any full python-suite run (acceptance, regression hunts) | `tools/run_python_suite.sh` (log dir arg optional) | Suite costs minutes: full output goes to a timestamped log under `build/test-logs/`, failure names reprint at the end. Never bare `discover | tail` — a truncated pipe hides the failure and forces a re-run |
| `time_suite.py` | Suite-speed work only, machine otherwise idle | `python3 tools/time_suite.py [log]` | One instrumented run, slowest-first table; concurrent suites skew the numbers, so never alongside another run |
| `dar.py` | Inspecting or patching DAR archives (asset work) | Extract, edit, `dar.py ... --from` (see `docs/formats/DAR.md`) | Size-preserving in-place rewriter: patches entry payload slots only, never rebuilds, so offsets and absolute references stay valid |
| `textures/repack_font_atlas.py` | Dialog-font atlas work at Nx scale | Glyph JSON in, TPL + metadata out (see tool docstring) | Overlays supplied glyph bitmaps on the decoded atlas and re-encodes; proves byte-identical round-trip before writing anything new |
| `embed_shaders.py` | Vulkan backend shader updates | `embed_shaders.py OUT_CPP OUT_HPP NAME=FILE.spv ...` | Embeds SPIR-V modules as C++ arrays for the backend build |
| `interpolation_report.py` | Diagnosing frame-interpolation artifacts from a bounded frame dump | `python3 tools/interpolation_report.py <dump.bin> [--verbose] [--vertex-only]` | Mirrors the runtime's pairing/rejection rules; reports bucket order breaks, rigid translations and collapsing triangles. `--vertex-only` shows what the shape-change rule prevents. Input is a `--es dump_draws` (guest frames) or `--es dump_presents` (interpolated presents) capture — both gitignored. An artifact visible only between guest frames requires the presented dump |
| `camera_trace_report.py` | Camera-hook research: reading a `GUNGNIR_TRACE_CAMERA` log | `python3 tools/camera_trace_report.py <run.log> [--sites]` | Groups GE commands 0x3E/0x3F into 16-lane projection matrices and decodes f24; reports distinct matrices and upload sites with instance counts. Input comes from a local bounded boot run (gitignored) |
| `camera_transform_report.py` | Deciding whether GE transform matrices carry per-draw motion | `python3 tools/camera_transform_report.py <run.log>` | Groups world (0x3A/0x3B), view (0x3C/0x3D) and bone (0x2A/0x2B) commands into 12-lane blocks, decodes f24, and counts DISTINCT matrices and upload sites. A title that projects vertices on the CPU shows only a handful of constant matrices — decisive for whether matrix interpolation is applicable. Input is a local bounded boot log (gitignored) |

## Rules

- Prefer the existing tool over reimplementing its format/codec/protocol
  (root `AGENTS.md` hard limit 0); extend the tool, don't clone it.
- Tool changes that alter outputs need the tool's own regression test green
  first (e.g. digest re-pin → `test_license_machine`; DAR writer change →
  round-trip test).
- Keep this table current: a tool without a row is a review finding.

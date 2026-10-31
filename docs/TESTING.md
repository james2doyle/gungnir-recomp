# TESTING — oracle hierarchy and fixture rules (T6 state)

Policy: plan §8.1 (red-first) and §8.2 (oracle hierarchy). This file covers
the T4 core harness, the T5 profile gates, and the T6 HLE fixtures.

## Oracle hierarchy

1. **Hand-computed expectations** — values derived by hand from the instruction
   encoding and written into the test driver (`tests/test_core_fixtures.py`,
   `HAND_COMPUTED`). Never generated from the implementation.
2. **The in-repo interpreter** — the semantic reference tier. Every fixture
   runs twice: interpreter tier vs AOT tier; the function-boundary traces and
   final architectural digests must be **byte-identical** (TC-4.1).
3. **Synthetic golden traces** — the fixture trace files themselves; any
   semantic mutation (opcode semantics, dispatch, memory map) must break
   equality.
4. **PPSSPP headless diff** — optional later bring-up oracle, test-only,
   never linked (plan §7F.7).
5. **On-device manual QA** — mandatory for device-facing milestones (plan §8.3).

## Red-first loop

- Bug fixes start from a failing fixture in `tests/core/fixtures/` whose stop
  reason or digest pins the bug (examples already recorded: ADDI decoding,
  scratchpad access, the VFPU forms in `vfpu_boot_gap.mips`).
- A fixture is added red, observed failing for the named reason, then fixed;
  the observation is recorded in the task/commit notes.
- Mutation checks: for codegen/HLE semantic changes, break one semantic and
  confirm the trace suite dies for that reason (TC-5.3 formalizes this for the
  Gungnir profile).

## Fixture format (`tests/core/fixtures/*.mips`)

```
# name: <fixture>
# sentinel: <absolute PC the fixture returns to; harness registers it>
# sp: <initial $sp>
# mem: <addr> = <hex bytes>      # initial guest memory (repeatable)
# digest: <start>:<end>          # memory windows included in state digests
<0xWORD>  # comment
...
```

Fixtures are linked as plain executables at `0x08804000` (one absolute
PT_LOAD), so guest literals are real runtime addresses. `assemble.py` emits the
ELF + functions CSV + metadata into `build/` — no binary fixture is ever
tracked (repository_check would reject ELF blobs; `.gitignore` covers build/).

## Trace format

Per dispatch at a declared trace PC (and at the sentinel):

```
start <pc> <sha256(state)>
pre   <pc> <sha256(state)>
end   <stopped|budget> <pc> <sha256(state)>
final-regs r0=… r31=… hi=… lo=…
final-mem <addr> <hex bytes…>
reason <verbatim runtime stop reason>
```

`state` covers gpr[32], hi/lo, fpr bits, vfpu bits, vfpu_ctrl, and the
fixture's declared memory windows. Stops exit 4 (loud); clean fixture
completion exits 0; an exhausted dispatch budget exits 3 — never silent
success.

## Profile gates (T5)

`tests/test_profile.py` adds four gates on the real local module. The suite
skips when the private disc is absent; it never passes silently.

- TC-5.1 `TestAnalysisManifest`: accounting identity
  (`analyzed + unreached == executable words`), census bounds asserted by the
  tool, required discovery sources present, 215/215 import stubs mapped,
  deterministic manifest bytes.
- TC-5.2 `TestCodegen`: every executable word is a registered entry, import
  wrappers match the inventory, repeated codegen is byte-identical and rewrites
  zero units, and the whole generated corpus compiles.
- TC-5.3 `TestMutationGate`: changing one generated fixture semantic
  (`+1` -> `+2`) must make the interpreter/AOT traces diverge with a changed
  guest register. A build error does not count; the mutation must kill the
  gate for the right reason.
- TC-5.4 `TestDispatchCoverage`: a bounded real-module run classifies every
  outer dispatch as translated or interpreter fallback; the negative control
  drops the entry unit and requires a nonzero, listed fallback count.

## HLE gates (T6)

- `tests/test_nids.py`: every Gungnir main-module import resolves to a name;
  CSV digest is pinned; PSPSDK provenance/license are recorded.
- `tests/test_hle.py`: cross-library symbol alias binds and invokes; an
  unbound import stops with `Missing HLE import … (NID_0x…)`, never a silent
  zero. `tests/hle/kernel_tests.cpp` covers memory partitions, thread
  lifecycle/preemption/delay, semaphores, event flags, blocking waits, VTimer
  interrupts, VPLs, lightweight mutexes, ISO disc + memory-stick IO, and async
  open/read/lseek/close staging through Wait/Poll/GetAsyncStat (busy, failure,
  no-async, closed-descriptor and bad-whence errors), and the sceUtility
  savedata SIZES state machine (filled usage blocks, missing-save NO_DATA, bad
  param size and path traversal).
  `tests/hle/media_tests.cpp` covers display mode/flip/vblank/multi-wait/
  frame-rate, controller sampling and vblank-blocking reads, GE callback and
  list completion, EDRAM, and the RTC tick. `tests/hle/module_tests.cpp`
  covers module lookup, stop/unload errors, and a corpus install → unload →
  different-corpus reload cycle with a stable function count.
- `tests/test_boot.py` (TC-6.2): builds the real-module boot runner against
  the T5 corpus and requires module init to reach a non-zero-width
  `sceDisplaySetFrameBuf`, with every executed import bound and no missing or
  silent NID. Two `--no-stop-at-flip` runs must produce identical deterministic
  fields (dispatches, virtual time, vblanks, flips, import lists, stop
  reason/PC) — TC-6.4. The determinism runs are bounded by 200,000 dispatches,
  not wall time, because the title now enters its main loop.

## Commands

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
python3 -m unittest tests.test_core_fixtures -v     # TC-4.1..TC-4.4
python3 -m unittest tests.test_profile -v           # TC-5.1..TC-5.4
python3 -m unittest tests.test_nids tests.test_hle -v  # T6a/T6b registry + kernel/HLE
python3 -m unittest discover -s tests -p 'test_*.py' -v   # full battery
```

Golden-file tests (post-processing pins) and PPSSPP diffs arrive with T7;
device QA with T13.

## GE renderer gates (T7)

`tests/test_ge_suite.py` is the consolidated TC-7.1–TC-7.4 gate: it runs the
display-list, vertex, texture, software-rasterizer, Vulkan and post fixtures
and checks the renderer/shader documents. Synthetic fixtures only.

- TC-7.1: commands, vertex formats, textures, triangles/strips/fans/sprites,
  blend, scissor, depth, alpha test and stencil-lite.
- TC-7.2: software vs Vulkan readback, tolerance 1 LSB per 8-bit channel.
- TC-7.3: pinned post-preset hashes (default, integer, scanline, LCD-grid,
  FSR-lite).
- TC-7.4: texture-range and CLUT re-upload invalidation.

Mutation evidence and the remaining named gaps (full GE state machine -> draw
path wiring, DXT textures, Vulkan post passes, Android presentation,
validation-layer availability) are recorded in `docs/RENDERER.md`.

## Texture container gates (T8)

`tests/test_tpl.py` builds and runs `tex_tpl_tests`: synthetic TPL containers
(no game bytes) covering CLUT4 alpha ramps, RGB565 palettes, CLUT8 palettes,
odd widths, multi-texture containers, counted auxiliary resources and named
malformed-input failures. `docs/formats/TPL.md` records the layout and the one local named gap
(`debugFont.tpl`, pixel format `0x0108`). T8f consolidates TC-8.1–TC-8.4.

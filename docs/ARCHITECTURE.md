# ARCHITECTURE — runtime core, HLE and renderer (T8 state)

The runtime is the MIT PSPRecomp framework core (vendored at
`third_party/yakumo/`, pinned + digest-verified; seven recorded local patches)
plus project glue built around it from T5 onward: the profile/codegen driver,
the adapted kernel/HLE boot set, the GE front end and Vulkan backend with the
post-processing chain, and the texture dump/replace pipeline. The Android shell
(T9) adds SDL3 presentation, identity-gated game-data intake and lifecycle
recovery.

## Layers

```
tools/intake/          disc -> verified local tree (T3, our glue + vendored readers)
third_party/yakumo/    framework core: decoder, interpreter, guest memory,
                       ELF/PRX loader, program analysis, bounded dispatcher,
                       NID registry, SHA-256, deflate; CLI analyzer/codegen
runtime/hle/           adapted MIT kernel + HLE: cooperative scheduler,
                       ThreadMan/SysMem/IO, guest exit stubs, registrar
runtime/ge/            display lists, vertex/T&L, software oracle, textures,
                       Vulkan backend, swapchain presentation
runtime/post/          composable post passes and preset JSON
runtime/tex/           TPL decode/write, content-keyed dump, PNG replacements
runtime/platform/android/  game-data identity gate, asset stamps, thread tuning (T9c/T9e)
runtime/platform/adreno/   driver zip inspector, loader selection, Vulkan dispatch (T10b/T10c)
android/.../DriverStore.java  imported-driver inventory and selection file (T10d)
android/               Gradle + SDL3 Activity shell and JNI entry (T9)
tests/core/            synthetic Allegrex fixtures + interpreter-vs-AOT harness
                       (our authorship; .mips text sources, ELF built into build/)
tests/hle/             kernel/HLE contracts (memory, threads, sync, timers,
                       IO, display/GE/ctrl) and the bounded boot runner
```

Nothing under `third_party/` is hand-edited without a recorded patch in its
`PROVENANCE.md`. Generated guest code is build output, always under
`build/`, never tracked (enforced by TC-4.3).

## Guest memory map (as implemented)

| Region | Guest window | Size | Notes |
|---|---|---|---|
| Scratchpad | `0x00010000` | 16 KiB | local patch: `Region::Scratchpad`; AOT fast path declines it, slow path resolves |
| VRAM | `0x04000000` | 2 MiB × 4 mirrors | mirror collapse to 2 MiB physical |
| Main RAM | canonical `0x08000000` (user alias `0x08800000`) | 32 MiB (64 MiB option) | inline AOT fast path covers RAM only |
| Cache aliases | `0x8…`/`0xA…` | — | canonicalized (`& 0x1FFFFFFF`) before resolution |

EDRAM is the `0x04000000` VRAM window above; other system windows are out of
scope for user-mode guest code. Out-of-window accesses throw a named
`Guest memory access outside PSP RAM` error, never a silent zero.

## Dispatch contract

- Guest functions are registered per address with `Runtime::register_function`;
  generated units are produced by `yakumo_codegen` from an ELF plus a
  functions CSV (`name,address,size`), and registered en masse by the emitted
  `psprecomp::register_generated_functions(Runtime&)`.
- A dispatch miss is resolved by the (permanent) interpreter tier; slices are
  bounded by the interpreter budget so scheduling/heartbeat still happen.
  `PSPRECOMP_NO_INTERPRETER=1` restores the strict stop — used by tests to
  prove dispatch misses are loud.
- Missing/unsupported hooks (`set_runtime_missing_function_hook`,
  `set_runtime_unsupported_hook`) are the overlay-swap seam for T6
  (`sceKernelLoadModule` tiers); `unregister_functions(start,end)` drops a
  translated range before reuse.
- Installing dispatch/chain observer hooks disables direct unit chaining so
  every unit boundary becomes observable — the fixture harness relies on this
  for deterministic traces.

## HLE contract (T6)

- `Runtime::register_hle(library, nid, fn)` binds a handler. Profile code
  registers by `(library, name)` through `gungnir::hle::HleRegistrar`;
  names resolve through `configs/nids.csv`, with cross-library aliases
  accepted only when the NID is unambiguous.
- Generated import wrappers call `invoke_import_cached`; an unbound import
  stops with `Missing HLE import <library>::<name-or-unknown> (NID_0x…)`.
  Registration itself is optional per title; executed imports are the
  fail-closed gate.
- `runtime/hle/hle_module.*` owns the module registry and ModuleMgr stop/
  unload semantics. A module's corpus is registered on install and its whole
  address window is unregistered before reuse; this title imports no loader,
  so the registry starts with the main module only.
- `Kernel::install` owns the loader thread, guest exit/interrupt stubs, the
  cooperative scheduler (priority, delay, sema/eventflag/mutex/waiter),
  VTimer/vblank delivery, and partition memory. HLE calls run at outer
  dispatch level; blocking saves the caller context and switches to the next
  ready thread through the runtime identity hook.
- Interactive boots pace virtual time to 59.94 Hz (`16,683 µs`); unit
  fixtures and the bounded boot runner disable pacing for deterministic
  traces. `tests/hle/boot_runner.cpp` runs the real module from its entry
  with dispatch and wall-clock limits and stops at the first presentable
  frame buffer.

## Fixture pipeline (T4 proof)

```
tests/core/fixtures/<name>.mips   (our text format: header + hex words)
  -> assemble.py                  -> build/tests/core/<name>/{elf,csv,json}
  -> yakumo_codegen               -> <name>_gen.cpp (build output)
  -> compiled into core_harness_<name>
core_harness --mode interp|aot   -> trace (dispatch digests + final state)
tests/test_core_fixtures.py      -> byte equality, hand-computed checks,
                                     loud-failure cases, determinism (TC-4.1..4.4)
```

## Known gaps (counted, not silent)

1. **No FNV block cache** in the adopted interpreter tier: dynamic code is
   interpreted correctly but without a decoded-block cache. Recorded gap;
   scope: land with the first workload that actually executes guest code via
   the interpreter (T5/T6 dispatch-coverage data decides priority; before M3
   benchmarking at the latest).
2. **No scratchpad mirror at `0x00010001` byte aliases** beyond the window
   itself: the Allegrex maps the 16 KiB window directly; sub-word accesses are
   handled by the generic slow path.
3. ADDI shares ADDIU semantics (no overflow trap) — documented divergence in
   `third_party/yakumo/PROVENANCE.md`.

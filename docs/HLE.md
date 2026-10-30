# HLE — kernel and import coverage (T6)

Status labels: VERIFIED (test-backed), HYPOTHESIS (observed but not gated),
PLANNED (not implemented).

## Registry (VERIFIED, T6a)

`configs/nids.csv` is generated from the pinned PSPSDK import stubs
(`c3da4314`, BSD-3-Clause) by `tools/nids/generate.py`; 19 NID/name pairs
missing from PSPSDK master are a recorded supplement (public interface facts,
cross-checked against PPSSPP/UOFW tables). The CSV is committed so builds need
no network.

| Figure | Value |
|---|---:|
| Registry symbols in CSV | 613 |
| Gungnir main-module imports | 215 |
| Imports resolved by `(library, NID)` | **215 / 215** |
| Libraries | 23 |

The runtime registry keeps its built-in entries and merges the CSV. Handlers
bind by `(library, symbol name)`; a name that exists under another alias
library (for example `Kernel_Library` importing a ThreadMan routine) binds
under the importing library when the NID is unambiguous. Distinct NIDs for one
name are refused as ambiguous.

## Unknown-import policy (VERIFIED)

Profile registration may include handlers this title does not import; those
are skipped. The contract is the executed import: an unbound import stops with

```
Missing HLE import <library>::<name-or-unknown> (NID_0x…)
```

It never returns a silent zero. `tests/test_hle.py` covers the named trap, the
cross-library alias path, and a bound handler invocation.

## Boot set

| Area | State | Notes |
|---|---|---|
| Registrar + NID registry | VERIFIED (T6a) | `runtime/hle/hle_common.*` |
| Kernel (cooperative threads, semas, eventflags, mutexes, VTimers, memory partitions) | VERIFIED (T6b) | adapted MIT Yakumo kernel; deterministic unpaced clock for tests |
| ThreadMan / SysMem / IoFileMgr | VERIFIED (T6b) | TC-6.1 unit fixtures: memory, create/start/exit/delay, sema/eventflag blocking, VTimer interrupt, ISO disc read + memory-stick write/read |
| LoadExecForUser / StdioForUser / scePower | VERIFIED (T6b) | exit callback, stdin/stdout/stderr, power callback + clock | 
| Display / GE / Ctrl | PLANNED (T6c) | first `sceDisplaySetFrameBuf` is TC-6.2 |
| sceRtc | PARTIAL | local clock implemented; Gungnir imports `sceRtcGetCurrentTick`, still pending T6c |
| ModuleMgr PRX overlay tiers | PLANNED (T6d) | translate-on-load, unregister-on-unload (TC-6.3) |

## T6c references rechecked (2026-10-30)

The MIT Yakumo tree has no separate display/GE/controls HLE file: those
handlers live in `profiles/mhp3rd/host/hle/hle_media.cpp`, together with
audio/renderer/input code. T6c will adapt only the timing/callback semantics
(sampling, vblank waits, GE list completion, EDRAM address/size) and keep the
renderer/audio dependencies out until T7/T12.

Gungnir's actual import set for the next stage:

- `sceDisplay`: SetMode, SetFrameBuf, WaitVblankStart, WaitVblankStartCB,
  WaitVblankStartMultiCB, GetVcount, GetFramePerSec.
- `sceGe_user`: EdramGetAddr/GetSize/SetAddrTranslation, Set/UnsetCallback,
  ListEnQueue/EnQueueHead/UpdateStallAddr, ListSync, DrawSync, Break, Continue.
- `sceCtrl`: SetSamplingMode/Cycle, SetIdleCancelThreshold,
  ReadBufferPositive.
- `sceRtcGetCurrentTick` is imported but not yet handled.

## Provenance

The kernel/HLE design is adopted from TeamGDB/Yakumo (MIT, pinned
`b61852a`), with profile-specific pieces removed and recorded in
`third_party/yakumo/PROVENANCE.md`. NID data provenance is in
`third_party/pspsdk-nids/PROVENANCE.md`. No emulator code is linked.

## Kernel/HLE fixtures (T6b)

`tests/hle/kernel_tests.cpp` exercises the real handlers through
`Runtime::invoke_import` on a 32 MiB runtime:

- partition allocation/free/coalescing and invalid-type rejection;
- thread create/start/priority preemption/exit/delete, GP inheritance and a
  virtual-clock delay (`sceKernelDelayThread`);
- semaphore acquire/poll/signal/overflow/delete;
- event-flag set/poll/clear/delete;
- a blocked waiter resumed by a signal;
- VTimer expiry delivered through the interrupt path;
- `disc0:/PSP_GAME/PARAM.SFO` open/read/close and a memory-stick
  create/write/close/reopen round-trip.

Pacing is disabled in unit fixtures (`Kernel::set_pacing_enabled(false)`) so
two runs see identical virtual time; interactive boots keep 59.94 Hz pacing.

## Commands

```sh
python3 -m unittest tests.test_nids tests.test_hle -v
```

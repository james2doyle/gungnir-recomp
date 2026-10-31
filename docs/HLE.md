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
| VPL / lightweight mutex (T6c boot discoveries) | VERIFIED | create/delete/try-allocate/free/refer pools; work-area LW mutexes shared by ThreadMan and Kernel_Library aliases |
| MsgPipe / FPL / thread-end (T6e boot discoveries) | VERIFIED | byte-stream pipes with blocking/try send and receive, timeouts, CB variants, cancel and status; fixed pools with blocking/try/CB allocate, free, cancel and status; `sceKernelWaitThreadEnd`/`CB`; `sceKernelWaitSemaCB` |
| sceAudio output channels (T6f boot discoveries) | VERIFIED | reserve/release, blocking and non-blocking single/panned output, both rest-length calls, data length, config and volume with the hardware check order; one buffer per channel, one parked waiter, 44.1 kHz virtual drain; explicit host sink seam (the boot runner records buffers) |
| sceSasCore (T6g boot discoveries) | VERIFIED | MIT Yakumo VAG/ADPCM software mixer (32 voices, pitch, volume, simple ADSR, pause, grain/output mode, end flag, envelope heights); init validation and hand-computed VAG grain contract; reverb/ATRAC3/noise voices remain unbound and stop loudly |
| sceAtrac3plus (T6h boot discoveries) | VERIFIED (ID lifecycle) | GetAtracID/Release with AT3 (0x1001) and AT3+ (0x1000) codec types; the FFmpeg n4.4 ATRAC3/3+ decoder subset is vendored with a public 4,149-check self-test. Track parse/decode handlers are added only as the title executes them; no decode call has run at this depth |
| Async IoFileMgr (T6i boot discoveries) | VERIFIED | Open/read/write/lseek/close async variants plus poll, wait, waitCB and getAsyncStat; host I/O completes synchronously but the result is staged per descriptor and delivered only by Wait/Poll/GetAsyncStat, with one outstanding operation and kernel error codes. `sceIoLseek32Async` is covered too |
| sceUtility savedata (T6k boot discoveries) | VERIFIED (SIZES) | InitStart/Update/GetStatus/ShutdownStart state machine; SIZES fills the guest `msFree`/`msData`/`utilityData` blocks from the virtual memory-stick directory and reports `NO_DATA` for a missing save. MsgDialog, GamedataInstall and the other savedata modes remain unbound/unsupported and are counted gaps |
| LoadExecForUser / StdioForUser / scePower | VERIFIED (T6b) | exit callback, stdin/stdout/stderr, power callback + clock |
| Display / Ctrl | VERIFIED (T6c) | mode, frame buffer, vblank waits (incl. multi/CB), vcount, 59.94 Hz; pad reads block on the next vblank |
| sceGe_user | VERIFIED seam | EDRAM address/size/translation, callbacks, enqueue/sync/stall/draw-sync, break/continue |
| sceRtc | VERIFIED | `sceRtcGetCurrentTick` plus local wall clock, both advancing with the virtual PSP clock |
| ModuleMgr (title-scoped) | VERIFIED (T6d) | module registry, GetModuleId/ByAddress, Stop/Unload, self stop-unload; no loader or overlay corpora in this title |
| ModuleMgr PRX overlay tiers | PLANNED (T6d) | translate-on-load, unregister-on-unload (TC-6.3) |

## Bounded boot evidence (T6c, VERIFIED)

`tests/hle/boot_runner.cpp` loads the decrypted main module at `0x08804000`,
registers the AOT corpus and the boot HLE, and runs from the entry with
dispatch and wall-clock bounds (`tests/test_boot.py`, TC-6.2).

| Figure | Value |
|---|---:|
| Module / entry | `9th`, `0x08972544` |
| Load base / image end | `0x08804000` / `0x08B37DCC` |
| Bound imports at this boot depth | 158 / 215 |
| Executed imports | 85 (all bound) |
| Missing / silent NIDs | 0 |
| First real flip | `0x04088000`, width 512, format 3 |

The first `sceDisplaySetFrameBuf` call is a zero-width display-off call from
module init; the gate reports only a buffer with a non-zero width. Two
bounded runs with the same inputs produce identical dispatch, virtual-time,
import and stop fields (TC-6.4).

Deeper execution (after four vblanks) passes the former VFPU gap (T5a) and the
ThreadMan object gap (T6e). The corpus contains no unlowered VFPU words, and
every import executed at this depth is bound. The deterministic 200,000-dispatch run reaches 2,178 vblanks, 738 flips and
36,345,296 us of virtual time, with 158/215 imports bound and 85 executed,
zero missing or silent NIDs; it stops at the dispatch bound (`0x0899C3DC`),
not at an HLE gap. The executed set includes `__sceSasInit`, `__sceSasCore`,
`sceAudioOutputPannedBlocking`, `sceAtracGetAtracID`, the async I/O path
(`sceIoLseekAsync`, `sceIoReadAsync`, `sceIoPollAsync`) and the savedata state
machine (`sceUtilitySavedataInitStart`, `Update`, `GetStatus`,
`ShutdownStart`). The title is in its main loop. A 30-second unpaced run
reaches 1,810,000 dispatches and 7,394 flips before the wall-clock bound.
Details are in `BRINGUP.md`.

## T6 completion audit (T6o, VERIFIED)

`tests/hle/boot_runner.cpp` names the module-init gate and reports the stop
cause and the kernel thread table. The gate is
`module_init_complete = flips > 0 && buffer_width != 0 && no missing import`:
a real presented frame proves module_start returned or the title reached its
own loop. The deterministic 200,000-dispatch run reports `true` /
`dispatch_limit` and the thread table:

```
uid=258 user_main              waiting wait=vblank
uid=301 SOUND thread(sas)      running
uid=306 at3mem thread          waiting wait=delay
uid=316 SOUND thread(UMD)      waiting wait=delay
```

Two 200,000-dispatch runs produce identical reports (TC-6.4). A 30-second
unpaced run reaches 1,810,000 dispatches and 7,394 flips without a missing
import.

The remaining 57 imports are **counted gaps**, not stubs: any that executes
still stops with `Missing HLE import …`. They are grouped as follows.

| Group | Imports | Disposition |
|---|---:|---|
| IoFileMgr `sceIoDread`/`sceIoIoctl` | 2 | implement with the first directory-scan/device-control use |
| Kernel/ThreadMan/Utils/UMD small calls | 8 | `WaitEventFlagCB`, profiler, `KernelMemcpy`, GPI, UMD medium/waits |
| sceUtility dialogs/install | 9 | MsgDialog (4) and GamedataInstall (5); savedata SIZES is implemented |
| ATRAC track | 6 | decoder vendored and self-tested; handlers wait for the first decode |
| SAS controls | 8 | ADSR/noise/reverb; wait for the first non-default control use |
| sceMpeg | 24 | no movie at boot depth; decision-gated for M5/T12 |

Other savedata modes (LOAD/SAVE/LIST/...) are modes of the four bound
savedata imports; they currently return a named error rather than fake
success and move to T12/M5 with the first non-SIZES use. T6 closes on this
audit; T7 remains required for M3 because no GE frame is rendered yet.

## Module registry (T6d, VERIFIED)

Gungnir imports five ModuleMgrForUser calls and none of them loads a module:
GetModuleId, GetModuleIdByAddress, StopModule, UnloadModule and
StopUnloadSelfModuleWithStatus. The disc ships no PRX files (only the
executable, BOOT.BIN, the excluded updater and DAR archives), so this title is
single-module and there are no overlay corpora to build.

`runtime/hle/hle_module.*` therefore keeps a registry of modules (id, name,
base, size, gp, started) rather than inventing a loader:

- `register_main_module` records the boot image before guest code runs.
- `add_module` is the translate-on-load seam: AOT translation happens offline
  in T5, so the module supplies a corpus installer that registers its
  functions in the module window. Unload calls
  `Runtime::unregister_functions(base, base + size)` before the address space
  can be reused.
- `tests/hle/module_tests.cpp` installs corpus A at a slot, stops it, unloads
  it, then loads corpus B in the same slot: the old function is gone, the new
  one is present, and the function count returns to baseline. Main-module
  unload is refused with `ILLEGAL_PERM`; unknown ids report `UNKNOWN_MODULE`.

Counted gap: `sceKernelLoadModule`/`StartModule`, module_start/module_stop
guest exports and per-thread module ids are not implemented. The title does
not import them; if a future title executes a load import it stops loudly with
its NID. Revisit when a real PRX/overlay source appears.

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
  Read/PeekBufferPositive, Read/PeekLatch. T11a feeds the pad from
  `runtime/input/` (button bits, merged analog, make/break latch); the latch
  samples on each positive-buffer read.
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
  create/write/close/reopen round-trip;
- async `open`/`read`/`lseek`/`close` staging through Wait/Poll/GetAsyncStat,
  the one-outstanding-operation busy error, failure results, and the
  no-async/closed-descriptor errors.

Pacing is disabled in unit fixtures (`Kernel::set_pacing_enabled(false)`) so
two runs see identical virtual time; interactive boots keep 59.94 Hz pacing.

## Commands

```sh
python3 -m unittest tests.test_nids tests.test_hle -v
```

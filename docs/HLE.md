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
| sceAudio output channels (T6f boot discoveries) | VERIFIED | reserve/release, blocking and non-blocking single/panned output, both rest-length calls, data length, config and volume with the hardware check order; one buffer per channel, one parked waiter, 44.1 kHz virtual drain; explicit host sink seam (the boot runner records buffers). A blocking output that switches threads leaves the resumed thread's registers untouched (the result belongs to the parked thread) |
| sceSasCore (T6g boot discoveries) | VERIFIED | MIT Yakumo VAG/ADPCM software mixer (32 voices, pitch, volume, simple ADSR, pause, grain/output mode, end flag, envelope heights); init validation and hand-computed VAG grain contract; reverb/ATRAC3/noise voices remain unbound and stop loudly |
| sceAtrac3plus (T6h boot discoveries) | VERIFIED (ID lifecycle) | GetAtracID/Release with AT3 (0x1001) and AT3+ (0x1000) codec types; the FFmpeg n4.4 ATRAC3/3+ decoder subset is vendored with a public 4,149-check self-test. Track parse/decode handlers are added only as the title executes them; no decode call has run at this depth |
| Async IoFileMgr (T6i boot discoveries) | VERIFIED | Open/read/write/lseek/close async variants plus poll, wait, waitCB and getAsyncStat; host I/O completes synchronously but the result is staged per descriptor and delivered only by Wait/Poll/GetAsyncStat, with one outstanding operation and kernel error codes. `sceIoLseek32Async` is covered too |
| sceUtility savedata (T6k boot discoveries, T13i list-save) | VERIFIED (SIZES, LIST, LISTLOAD, LISTSAVE) | InitStart/Update/GetStatus/ShutdownStart state machine; SIZES fills the guest `msFree`/`msData`/`utilityData` blocks from the virtual memory-stick directory and reports `NO_DATA` for a missing save. LIST fills `SceUtilitySavedataIdListInfo` from the save directories matching `gameName + saveName` (`*`/`?` wildcards, sorted, game prefix stripped). LISTLOAD (mode 4, executed by the new-game path with a 32-slot `saveNameList`) resolves candidates from the list or the single save name, applies the `focus` policy (LATEST/OLDEST use the `PARAM.SFO` mtime), auto-cancels an empty list with `NO_DATA`, and otherwise writes the selected name back and runs the LOAD path. LISTSAVE (mode 5, proven by Gungnir's system-menu save on device) honors the caller focus entry of `saveNameList` with PPSSPP's `SavedataParam` fallbacks (NAME index, FIRST/LASTLIST, LATEST/OLDEST by mtime, FIRST/LASTDATA/EMPTY, everything unknown falls back to the first entry), writes the selected name back, and runs the SAVE path. LISTDELETE, GamedataInstall and the remaining savedata modes are counted gaps |
| sceUtility message dialog | VERIFIED (headless) | The same status machine with a documented headless auto-accept: once the dialog is visible it completes immediately with result 0 and `buttonPressed = YES`, so a title that expects a player acknowledgement continues without a renderer. The V1/V2/V3 parameter sizes are accepted; anything else fails closed |
| LoadExecForUser / StdioForUser / scePower | VERIFIED (T6b) | exit callback, stdin/stdout/stderr, power callback + clock |
| UtilsForUser cache and GPO calls | VERIFIED (T13i) | dcache/icache flushes are host no-ops; `sceKernelSetGPO` accepts the LED mask and `sceKernelGetGPI` reports 0, matching retail hardware with no dev-kit switch bank |
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

The remaining 15 imports are **counted gaps**, not stubs: any that executes
still stops with `Missing HLE import …`. They are grouped as follows.

| Group | Imports | Disposition |
|---|---:|---|
| IoFileMgr `sceIoDread`/`sceIoIoctl` | 2 | implement with the first directory-scan/device-control use |
| Kernel/ThreadMan/Utils small calls | 4 | `WaitEventFlagCB`, profiler, `KernelMemcpy`, GPI |
| sceUtility dialogs/install | 9 | MsgDialog (4) and GamedataInstall (5) |

The eight SAS controls (`__sceSasSetADSR`, `SetADSRmode`, `SetSL`, `SetNoise`,
`RevType`, `RevParam`, `RevEVOL`, `RevVON`) are now bound with the PSP
validation and error codes. Long ADSR rates and curve modes feed the existing
envelope; decay is always exponential because the simple envelope encoding has
no linear/instant decay, which is recorded as a limitation. Noise voices play
silence (the reference emulator does not synthesise noise either) and reverb
settings are stored but not mixed. The first NEW GAME transition executed
`__sceSasSetADSRmode`, which used to stop the session.

The UMD medium/wait calls (`sceUmdCheckMedium`, `sceUmdWaitDriveStat(CB)`,
`sceUmdCancelWaitDriveStat`) are now bound: the drive is reported present and
ready. Savedata modes LOAD, SAVE, SIZES, AUTOLOAD and AUTOSAVE are implemented;
LIST, DELETE and the save picker remain named gaps.

sceMpeg is not an unbound gap any more: all 24 imported entry points are bound
to explicit errors (`SCE_MPEG_ERROR_NO_RAPI` from `sceMpegInit`,
`NOT_INITIALIZE` afterwards, cleanup success) and log one
`[hle] gap: sceMpeg::…` line. The guest therefore skips the opening movie and
reaches the title; actual PMF playback remains decision-gated for M5/T12.

`sceAtracDecodeData` waits for the media engine on every decode attempt,
including an empty buffer: `1815 + bytes * 158 / 100` µs for stereo ATRAC3+
and `970 + bytes * 44 / 100` for stereo ATRAC3 (the PPSSPP hardware fit).
Without that wait the priority-16 sound thread spun on `BUFFER_IS_EMPTY`
after its first 128 KiB of streamed data and starved `user_main`, so the feed
thread never ran and the title stopped presenting frames. The contract is
`an empty decode still accounts for the media-engine frame time` in
`tests/hle/kernel_tests.cpp`.

`sceKernelCheckCallback` with no queued callbacks yields for 50 µs. Without
that idle yield the post-movie worker (priority 31) busy-polled
`TryReceiveMsgPipe` + `CheckCallback` and starved `user_main` (priority 32), so
the game never sent the worker's completion event. The contract is
`check_callback_yield_contract` in `tests/hle/kernel_tests.cpp`; it fails when
the yield is replaced by an immediate return.

T6 closes on this audit; T7 remains required for M3 because no GE frame is
rendered yet.

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

Interactive boots hold the virtual clock to real time at a host speed multiple
(`Kernel::speed()`): 1.0 nominal, `kFastForwardSpeed` (3.0) while fast-forward
is held or latched. `Kernel::pace_step()` is the pure decision (how long to
sleep, whether to drop the debt), so the multiple is tested without sleeping;
a speed change rebases the hold instead of charging the new multiple for time
already spent. The vblank cadence, VTimers and every emulated deadline are
unchanged: fast-forward buys wall time, not guest frames, and a snapshot never
records the multiple.

## Commands

```sh
python3 -m unittest tests.test_nids tests.test_hle -v
```

### Diagnostics

`GUNGNIR_TRACE_SYNC=1` logs kernel synchronization activity;
`GUNGNIR_TRACE_IO=1` logs disc/memory-stick I/O; `GUNGNIR_TRACE_SAVEDATA=1`
logs savedata init/status/list operations (limit with
`GUNGNIR_TRACE_SAVEDATA_LIMIT=N`). `GUNGNIR_HLE_RING=N` keeps the last N HLE
import calls with `a0`-`a3`, `v0`/`v1` and the return PC, and the session
prints them when a run stops on a fault. `GUNGNIR_CHAIN_RING=N` does the same
for dynamic `invoke_chained_call` frames (target, depth, return PC, `a0`-`a3`,
thread, in-flight markers), which is how a fault deep inside one outer
dispatch is attributed to its callers. `PSPRECOMP_WATCH_WRITE=0xADDR` with
`PSPRECOMP_WATCH_WRITE_SIZE=bytes` arms a guest write watch and logs every
overlapping store with its dispatch PC, thread and old/new value (narrow the
range; each store prints a line). All are opt-in and add no work when unset.

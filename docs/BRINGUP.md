# BRINGUP — findings journal

Living log for bring-up work. One entry per symptom: symptom → falsification →
root cause → regression test. Retractions are recorded, not deleted. Evidence
is labeled VERIFIED, HYPOTHESIS, or PLANNED.

## 2026-10-30 — T5: stripped section names hid the import table

**Symptom.** `yakumo_analyzer` reported `module: <not found>`, `imports: 0` on
the main EBOOT, while the T3 intake manifest listed 215 imports for the same
decrypted ELF. Codegen consequently emitted `import_wrappers: 0`.

**Falsification.** `readelf -S` showed 55 section headers with all names blank
(`e_shstrndx` is a zero-length string table). The name-based
`.sceModuleInfo` lookup in the vendored reader can therefore never match. The
intake tool had a caller-side content scan and found imports because it did not
use the named lookup.

**Root cause.** Sectionless/stripped commercial PRX: module-info discovery must
be content-anchored. With no module info, codegen does not recognize import
stubs; each stub is `jr $ra`, so a call would return silently — exactly the
hidden-failure class the plan forbids (gotcha C.1/C.8).

**Fix.** Local patch in `third_party/yakumo/src/elf32.cpp`: content-anchored
56-byte header scan, module-relative field normalization, functional
validation through `scan_imports()`. VERIFIED: 215/215 stubs mapped, 215 HLE
wrappers emitted, and an import call now stops with `Missing HLE import …`.

**Regression.** `tests/test_profile.py::TestAnalysisManifest::test_import_inventory_is_fully_mapped`
and `TestCodegen::test_import_wrappers_match_inventory`.

## 2026-10-30 — T5: HI16/LO16 pairing found nothing

**Symptom.** With pair seeding implemented, the census gate failed:
`required discovery source 'hi16_lo16_pair' produced no seeds`.

**Falsification.** A relocation dump showed 544 `R_MIPS_HI16` + `R_MIPS_LO16`
`addiu` pairs resolving into executable ranges, and 2,999 pairs into data.
Pairing was working; the seed map was not attributing them.

**Root cause.** `add_seed` keeps the first source for an address. The
materialized-pointer pass ran first and had already claimed every target, so
the pair pass added nothing with its own label.

**Fix.** Run the authoritative relocation pass before the heuristic
materialized-pointer scan. The discovered address set is unchanged; attribution
now shows 282 pair seeds (the rest already had direct `jal` or R_MIPS_32
sources). VERIFIED.

**Regression.** `TestAnalysisManifest::test_required_discovery_sources_present`.

**Retraction.** The first implementation note claimed the pair pass added no
new addresses. Correct: it adds no addresses the other passes miss on this
dump; its value is authoritative relocation coverage and future dumps where
straight-line propagation loses a pair. It remains a required gate.

## 2026-10-30 — T5: unreached executable words are data, not missing code

**Symptom.** Analysis reached 359,285 of 424,373 executable words (84.7%),
which looks like incomplete coverage.

**Falsification.** The linked image is one RWX `PT_LOAD`; 24
`SHF_EXECINSTR` sections total exactly 424,373 words and are contiguous. The
unreached 65,088 words lie inside executable sections but decode as data
(tables embedded between functions); codegen emits them as explicit trap
entries.

**Root cause.** Section flags describe mapping permissions, not content.
"100%" in TC-5.1 means every executable word is accounted for as analyzed
instruction or counted data/trap and registered as a dispatch entry — not that
every word is an instruction.

**Fix.** Manifest reports `words_analyzed`, `words_unreached`, and
`coverage: 1.0`; codegen asserts `registered_entries == words_total`. VERIFIED.

**Regression.** `TestAnalysisManifest::test_executable_bytes_fully_accounted`
and `TestCodegen::test_codegen_covers_every_executable_word`.

## 2026-10-30 — T5: unregister_functions left stale generated units dispatchable

**Symptom.** The TC-5.4 negative control dropped the entry unit with
`unregister_functions(unit_start, unit_end)`, but the run still reported the
entry dispatch as translated. `runtime.has_function(entry)` was already false,
so the coverage report contradicted the dispatch path.

**Falsification.** The normal dispatch loop consults the fast generated-unit
table before the per-PC map (`lookup_generated_unit` then `lookup_function`).
`unregister_functions` erased the map entries and direct-table slots but never
cleared `generated_units_`, so the unit pointer kept dispatching old code.

**Root cause.** Overlay swap path incomplete: a fully unregistered 16 KiB unit
must leave the fast unit table, or reused address windows execute stale
translated code while `has_function()` says the address is gone (plan gotcha
C.6). This is a hidden failure by construction.

**Fix.** Local patch in `third_party/yakumo/src/runtime.cpp`: when the
unregistered range fully covers a generated unit, null its fast-table pointer
and entry function and mark it disabled. VERIFIED: the negative control now
records one interpreter fallback at the entry PC followed by the translated
import-wrapper dispatch.

**Regression.** `tests/test_profile.py::TestDispatchCoverage::test_forced_fallback_is_counted_not_silent`
(and the local patch ledger, entry 6).

## 2026-10-30 — T6b: optional HLE registrations aborted the boot fixture

**Symptom.** The kernel fixture failed during registration with
`Unknown HLE function SysMemUserForUser::sceKernelAllocMemoryBlock` even
though Gungnir never imports that symbol.

**Falsification.** The adapted boot surface registers every handler the
MHP3rd profile needed, but Gungnir's import inventory is a 215-symbol subset;
PSPSDK-derived `configs/nids.csv` correctly does not contain every optional
handler name. The failure was registration-time, before any guest code ran.

**Root cause.** Treating the registration list as the coverage contract. The
real contract is the module's import inventory: a handler may be optional,
but an executed import without a binding must stop.

**Fix.** `HleRegistrar::add` treats a name absent from the registry as an
optional handler and skips it; `Runtime::invoke_import` remains the
fail-closed gate with the named NID trap. VERIFIED by the kernel fixtures,
which bind only what the CSV resolves and still exercise every intended path.

**Regression.** `tests/hle/kernel_tests.cpp` (fixture construction) and
`tests/test_hle.py::TestKernelHle::test_memory_threads_sync_and_io`.

**Follow-up.** T6c's boot runner must compare the module's full import
inventory against bound `(library, NID)` pairs before/at the end of the
bounded run, so an unwittingly skipped handler is counted rather than assumed
optional.

## 2026-10-30 — T6c: boot stops on missing VPL and lightweight mutex handlers

**Symptom.** After module entry, the bounded boot stopped at
`Missing HLE import ThreadManForUser::sceKernelCreateVpl (NID_0x56C039B5)`;
the next run stopped at `sceKernelCreateLwMutex (NID_0x19CFF145)`.

**Falsification.** Both NIDs resolve through `configs/nids.csv`, and Gungnir
imports the whole family (create/delete/try-allocate/free/refer VPLs;
create/delete/lock/unlock LW mutexes through both ThreadMan and
Kernel_Library aliases). The registrations, not the registry, were
incomplete.

**Root cause.** The adapted MHP3rd boot set never needed these objects, so
they were absent even though the interface names were known.

**Fix.** Added VPLs to `Kernel` (one partition block per pool, first-fit
64-byte-aligned guest allocations, free by address, status report) and LW
mutexes over the existing mutex scheduler with the guest work area mirrored
on every ownership change. `Kernel_Library` aliases register the same
handlers. The VPL blocking form reports no memory instead of parking the
caller; boot traffic uses the try form.

**Regression.** `tests/hle/kernel_tests.cpp` (`vpl_contracts`,
`lwmutex_contracts`) and the T6c boot suite.

**Follow-up.** The non-blocking `sceKernelAllocateVpl` shortcut must gain a
proper wait type if a later boot path depends on blocking allocation.

## 2026-10-30 — T6c: first display call is a display-off call

**Symptom.** Stopping on the first `sceDisplaySetFrameBuf` produced
`framebuffer=0x00000000 width=0 format=1`, before any vblank, which would
make a poor M3 evidence point.

**Falsification.** The guest calls `sceDisplaySetFrameBuf` during init with a
zero-width buffer to turn the display off; it sets the real 512x272 buffer
later in the same boot path.

**Root cause.** Treating every call as a presentable flip. The hardware call
also serves as display enable/disable.

**Fix.** The media state counts every call but only invokes the host flip
observer when `buffer_width != 0`. The boot gate now waits for
`0x04088000`, width 512, format 3.

**Regression.** `tests/hle/media_tests.cpp::display_contracts` and
`tests/test_boot.py::test_boot_reaches_first_frame_buffer`.

## 2026-10-30 — T6c next blocker: unlowered VFPU4 matrix operations

**Symptom.** Continuing past the first flip (four vblanks, 233 outer
dispatches) stops with
`Unsupported Allegrex instruction 0xD0428080 at 0x0899BD90: vfpu4 not lowered yet`.

**Falsification.** The instruction is valid Allegrex VFPU4 (group 2, function
2, a matrix operation), not data decoded as code. The generated corpus emits
an explicit `unsupported` for 33 such words across a handful of units
(`0xD03C8080`, `0xD0428080`, `0xD0420181`, `0xD05B8xxx`, `0x6701xxxx`).

**Root cause.** AOT codegen covers the VFPU forms exercised by the T4 fuzz
and fixture suite but not the VFPU4 matrix-init/sort encodings this title
uses. This is plan gotcha C.3 (missing encoding = hard runtime stop), not an
HLE gap.

**Fix.** None in T6c; the first-flip criterion is met before this address.
Tracked as a T5 follow-up and required before T6d/M3 can claim a deeper boot.

**Regression.** `tests/test_boot.py` pins the current first-flip boundary;
extending the boot run will expose the VFPU fix.

## 2026-10-30 — T6d: the plan's PRX overlay tier has no client in this title

**Symptom.** T6d's scope called for ModuleMgr PRX load/start/stop/unload and
translate-on-load overlay tiers, but Gungnir imports only five ModuleMgr
calls — GetModuleId, GetModuleIdByAddress, StopModule, UnloadModule and
StopUnloadSelfModuleWithStatus — and none of them loads anything. The disc
contains no PRX files: only EBOOT.BIN, BOOT.BIN, the excluded updater and DAR
archives.

**Falsification.** The import inventory (`build/game/manifest.json`) and a
recursive ISO listing both agree; the generated call sites only reference the
unload/stop stubs. A loader would be dead code with no corpus to load.

**Root cause.** The plan generalized from other PSP profiles. This title is a
single-module executable whose module calls are teardown paths.

**Decision.** Implement the title-scoped registry (recorded user sign-off):
module lookup, stop/unload, self stop-unload, and a corpus-install seam that
unregisters a module's whole address window before reuse. Record
load/start, module_start/module_stop exports and per-thread module ids as a
counted gap; an executed load import still stops loudly with its NID.

**Regression.** `tests/hle/module_tests.cpp`: corpus A installs at a slot,
stops, unloads, then corpus B installs at the same base; old registrations are
gone, the new one is present, and the function count returns to baseline.
Main-module unload returns ILLEGAL_PERM; unknown ids return UNKNOWN_MODULE.

**Follow-up.** Revisit the loader only when a title or overlay source actually
requires it; DATA.DAR remains T12 asset work.

## 2026-10-30 — T5a: the VFPU4/VFPU1 gap is fixed

**Symptom.** The deeper T6c boot stopped at `0x0899BD90` with
`Unsupported Allegrex instruction 0xD0428080: vfpu4 not lowered yet`; 33
words across the corpus emitted explicit `unsupported` calls.

**Falsification.** The words are valid VFPU encodings, not data: `vcrs`
(VFPU1 op 5), `vdet` (op 6), `vbfy1/2` (VFPU9 fn 2/3), `vi2uc/c/us/s`
(VFPU7 fn 28-31) and `vt4444/5551/5650` (VFPU9 fn 25-27). The decoder
collapsed every one into a generic `vfpu1`/`vfpu4` mnemonic.

**Root cause.** AOT codegen and the interpreter shared no lowering for these
forms, and a freshly created `AllegrexContext` started with zero S/T prefixes
instead of the hardware identity swizzle (`0xE4`). The latter made any
no-prefix VFPU hand check wrong even where a helper existed.

**Fix.** Classified the encodings, added shared `AllegrexContext` helpers
(`execute_vfpu_vcrs`, `_vdet`, `_vbfy`, `_vi2x`, `_color_convert`) and wired
both tiers. Prefix rewrites follow the hardware rules: VCrs forces S yzx? /
T zxy?, VBfy forces the upper-half negates and yxwz/zwxy, VDet forces T yx??
and negates S.y, while VI2x and colour conversion apply the source prefix
through a four-lane view. Fresh contexts now reset S/T prefixes to `0xE4`.

**Evidence.** The regenerated corpus has zero `not lowered yet` emissions.
The bounded boot now advances from 233 to 387 dispatches. The new fixture
`tests/core/fixtures/vfpu_boot_gap.mips` executes all families with
hand-computed memory results and passes both tiers.

**Next blocker (HLE, not codegen).** The same run now stops at
`Missing HLE import ThreadManForUser::sceKernelCreateMsgPipe
(NID_0x7C0DC2A0)`; message pipes, FPLs and the remaining ThreadMan wait
objects are the next boot-depth work.

## 2026-10-30 — T6e: ThreadMan message pipes, fixed pools and thread-end waits

**Symptom.** After the T5a VFPU fix the deeper boot stopped at
`Missing HLE import ThreadManForUser::sceKernelCreateMsgPipe
(NID_0x7C0DC2A0)` after 387 dispatches.

**Falsification.** The import inventory already maps the call and the encoder
is fine; the stop is HLE coverage, not codegen. The imported object family is
consistent: message pipes (create/delete/send/receive/try/CB/cancel/refer),
fixed pools (create/delete/allocate/try/CB/free/cancel/refer) and
`sceKernelWaitThreadEnd`/`CB`. The ABI contract came from the pinned PSPSDK
headers (BSD-3-Clause, already used for the NID registry); PPSSPP behaviour
was consulted only as a diff oracle, with no code copied.

**Root cause.** The kernel had no message-pipe or fixed-pool models, so the
first object creation stopped the import dispatcher. `sceKernelWaitThreadEnd`
was also missing even though the scheduler already had a `ThreadEnd` wait type
and exit woke those waiters.

**Fix.** `runtime/hle/kernel.*` now models a pipe as a byte stream with an
optional zero-size direct-transfer mode, FIFO/priority waiter order, timeout
deadlines and cancel wakeups, and a pool as equal aligned blocks carved from
one kernel memory block. Both handlers follow the hardware completion rules:
a full-mode transfer only copies when the whole request fits, an
ASAP-mode transfer may return short with the transferred byte count, receive
consumes what is available before blocking, and `CB` variants deliver pending
callbacks before waiting. Thread-end waits return the target's exit status and
now wake correctly from `exit` and `terminate`; the first version read the
waiter's own exit status and reported zero (caught by the contract).

The boot runner test harness cached object files against the `.cpp` mtime
only, so a header change produced a stale object linked against the new
library (observed as a crash in `kernel()`). It now invalidates on any runtime
or framework header change.

**Evidence.** `tests/hle/kernel_tests.cpp` adds message-pipe, fixed-pool and
thread-end contracts: creation/argument errors, status layouts, partial
transfers, blocked wakeups, timeouts with remaining-time clearing, cancel
counts, and unknown-id errors. Mutation check: removing the receiver's
sender-pump makes "freeing eight bytes wakes the sender" fail; restored code
passes. The bounded boot now advances 387 → 752 (WaitSemaCB) → 20,612
dispatches with 118/215 imports bound and 58 executed, all bound, zero
missing or silent NIDs. The executed set includes `CreateMsgPipe`,
`TryReceiveMsgPipe`, `CreateFpl`, `TryAllocateFpl` and `WaitSemaCB`, so the
new paths are exercised by the game, not only by unit fixtures.

**Next blocker (audio, not ThreadMan).** The run stops at
`Missing HLE import sceAudio::sceAudioChReserve (NID_0x5EC81C55)`; output
channel reservation/output is the next boot-depth task (T6f). Full SAS/ATRAC
synthesis remains T12.

## 2026-10-30 — T6f: sceAudio output channels

**Symptom.** After T6e the deeper boot stopped at
`Missing HLE import sceAudio::sceAudioChReserve (NID_0x5EC81C55)` after
20,612 dispatches.

**Falsification.** The import is bound in the inventory; the stop is HLE
coverage. The executed set also shows `sceAudioChangeChannelConfig` and
`sceAudioSetChannelDataLen`, so the title configures channels before it ever
outputs a buffer.

**Root cause.** No sceAudio state existed. The host also had no sink seam, so
accepting guest PCM would have been a silent stub.

**Fix.** `runtime/hle/hle_audio.*` models the eight mixer channels from the
hardware contract: one buffer per channel, one parked blocking caller, a
44.1 kHz drain scheduled on the virtual clock, both rest-length calls (they
differ after a null-buffer drain), broadcast vs panned volume rules and the
documented error-check order. `runtime/audio/audio_sink.*` is the host seam;
the boot runner installs an explicit counting sink, and a run without a sink
stops with `no audio sink registered` rather than dropping samples. This is
not Yakumo audio code: the device backend (SDL3/AAudio ring buffer, underrun
counters) remains T12, where the MIT Yakumo sink can be adapted if useful.

**Evidence.** `tests/hle/kernel_tests.cpp` adds audio contracts: reserve
errors, one-buffer busy, blocked output parked until the drain, both rest
counts (64 + parked 64 = 128), null-drain disagreement, mono/stereo staging,
volume/config/data-length check order, and release while a thread is parked.
Mutation check: returning `BUSY` instead of parking fails "blocking output
parks the caller". The bounded boot now reaches 20,639 dispatches with
127/215 imports bound and 61 executed, zero missing or silent NIDs; the
executed audio calls are reserve, change-config and set-data-length, with no
output buffer yet.

**Next blocker (SAS synthesis).** The run stops at
`Missing HLE import sceSasCore::__sceSasInit (NID_0x42778A9F)`. A bounded SAS
core slice (init, voice setup, grain render into a guest buffer) is the next
boot-depth task, tracked as T6g; full audio mixing/output remains T12.

## 2026-10-30 — T6g: sceSasCore software mixer

**Symptom.** After T6f the deeper boot stopped at
`Missing HLE import sceSasCore::__sceSasInit (NID_0x42778A9F)` after 20,639
dispatches.

**Falsification.** The import is bound in the inventory; the stop is HLE
coverage. The title imports 31 SAS entry points, so a real synth path is
required rather than a returning stub.

**Root cause.** No SAS core existed. A VAG/ADPCM synth is not something to
hand-roll under the codec rule, and Yakumo already carries an MIT one.

**Fix.** `runtime/audio/sas_core.*` adapts Yakumo's MIT
`profiles/mhp3rd/host/audio/sas_core.*` (recorded in the vendored provenance):
32 voices, SPU ADPCM decode, pitch resampling, simple ADSR, pause, grain and
output mode. Namespace and per-title environment/trace hooks were removed and
cores are keyed in runtime state. One behavioural fix: a non-looping source
now plays its final decoded sample; upstream stopped one sample early, which
the hand-computed grain caught. `runtime/hle/hle_audio.*` adds the handlers
with the documented init/voice/pitch/volume/ADSR error codes and check order.
Reverb, noise, steep-wave and ATRAC3 voices are not implemented and stay
unbound so they stop loudly.

**Evidence.** `tests/hle/kernel_tests.cpp` adds SAS contracts: init error
order, voice/pitch/volume/ADSR validation, a constant-nibble VAG block that
must yield 28 frames of 1023, post-terminator silence, end flag, envelope
heights, pause flag, grain/output-mode round-trip, and `CoreWithMix`
accumulation (100 + 1023 = 1123). Mutation check: restoring the upstream
one-sample-early stop fails "last decoded frame matches". The bounded boot
now reaches 22,086 dispatches with 146/215 imports bound and 64 executed,
zero missing or silent NIDs; `__sceSasInit`, `__sceSasCore` and the first
`sceAudioOutputPannedBlocking` (512 silent frames) all ran.

**Next blocker (ATRAC3+).** The run stops at
`Missing HLE import sceAtrac3plus::sceAtracGetAtracID (NID_0x780F88D1)`.
Decoding ATRAC3+ requires adopting an existing decoder
(`at3_standalone`-class) under the codec rule; that belongs to T12, not to a
home-grown implementation.

## 2026-10-30 — T6h: ATRAC3/3+ decoder adoption and ID lifecycle

**Symptom.** After T6g the deeper boot stopped at
`Missing HLE import sceAtrac3plus::sceAtracGetAtracID (NID_0x780F88D1)` after
22,086 dispatches.

**Reference review.** No permissively licensed ATRAC3+ decoder exists.
`libsoundwave` has no ATRAC support; `atracdenc` decodes only ATRAC1; vgmstream
delegates to libavcodec; ATRACTool is a tool. The two viable extractions are
PPSSPP's `ext/at3_standalone` and Nakagawa recomp's `src/rt/atrac3p`. Nakagawa
wins on provenance (FFmpeg n4.4 tag + per-file blob SHAs, documented deltas,
authored interleaved-s16 API, selftest and title-accept evidence, no PPSSPP
code). `hst-decomp` was also checked: it is the matching decompilation, not a
decoder, and its audio call sites are not decompiled yet.

**Root cause.** No sceAtrac objects and no decoder. Decoding ATRAC is an
existing-codec case under hard limit #0, so the fix is adoption, not a
clean-room implementation.

**Fix.** `third_party/atrac3p` vendors Nakagawa's subtree byte-identical at
`1117d635` (FFmpeg n4.4 decoder files, LGPL-2.1-or-later) with its manifest
preserved; one additive local delta adds `extern "C"` guards to the C API
header. The dependency ledger, notices and digest are updated.
`runtime/hle/hle_atrac.*` adds the ID lifecycle: GetAtracID accepts AT3 and
AT3+ types and Release frees the object; unknown IDs and types fail by code.
`tests/audio/atrac3p_selftest.cpp` adapts the public Nakagawa self-test
(4,149 checks) including the terminator frame and the IMDCT/IPQF transform
canary; determinism is checked by buffer comparison.

**Evidence.** `tests/test_audio.py` builds and runs the decoder self-test;
`tests/hle/kernel_tests.cpp` pins the ID lifecycle. The boot now reaches
23,343 dispatches with GetAtracID bound. Implementing
`sceKernelCheckCallback` (the next stop) advances it to 25,546 dispatches,
15 vblanks and 17 flips with 148/215 imports bound and 77 executed, zero
missing or silent NIDs. The executed ATRAC call is GetAtracID only; the title
has not asked for track data or decode at this depth.

**Next blocker (asynchronous I/O).** The run stops at
`Missing HLE import IoFileMgrForUser::sceIoLseekAsync (NID_0x71B19E77)`;
async completion for the IO manager is the next boot-depth task (T6i).

## 2026-10-30 — T6i: asynchronous IoFileMgr completion

**Symptom.** After T6h the deeper boot stopped at
`Missing HLE import IoFileMgrForUser::sceIoLseekAsync (NID_0x71B19E77)` after
25,546 dispatches.

**Reference review.** Code search for `IoFileMgrForUser` (the discovery step
now recorded in AGENTS.md) found PPSSPP's async I/O implementation and its
regression history (#12549, #12506, #136, #153), snuri00/psp-web-recomp's MIT
static-recompiler `profile/host/io.cpp`, and psce4all/psprism's generated
stub tables. PPSSPP defines the contract: `sceIoOpenAsync` returns a
descriptor immediately; every async operation stages a 64-bit result that
only Wait/Poll/GetAsyncStat makes visible; the descriptor stays busy until
the result is collected; close defers descriptor release to that collection.
psp-web-recomp independently confirms the wait/poll/close staging but
completes inline, which is the model used here.

**Root cause.** The IoFileMgr surface was synchronous-only. `sceIoOpen`,
`sceIoRead`, `sceIoLseek` and `sceIoClose` existed, but the async family was
unbound, and there was no per-descriptor result state or wait/poll path.

**Fix.** `runtime/hle/hle_io.cpp` now stages async results per `OpenFile`:
open/read/write/lseek/close set a result and mark the descriptor busy;
`sceIoWaitAsync`, `sceIoWaitAsyncCB`, `sceIoPollAsync` and
`sceIoGetAsyncStat` collect it and release a pending close. Host I/O is
synchronous, so the result is ready at the initiating call; there is no
sleep or wall-clock delay, and the pending branch still waits through the
kernel's host-wait seam for a future backend that completes later. Failed
async opens allocate a descriptor, return the error through Wait, and release
it on collection. Busy, no-async, closed-descriptor and bad-whence errors use
the kernel codes. `register_io` now clears the descriptor table so fixtures
start clean.

**Evidence.** Red-first `async_io_contracts` in `tests/hle/kernel_tests.cpp`
failed at "async open returns a descriptor immediately" before the handlers
were bound. It now covers open/read/lseek/close staging, poll and
getAsyncStat, waitCB, the busy error, a failed open, missing-async and
closed-descriptor errors, and invalid whence. The bounded boot advances to
26,058 dispatches, 21 vblanks and 19 flips with 154/215 imports bound and 82
executed, zero missing or silent NIDs. The executed async calls at this depth
are `sceIoLseekAsync`, `sceIoReadAsync` and `sceIoPollAsync`;
`sceIoOpenAsync`/`WaitAsync` are bound and tested but not executed by this
title yet. `sceIoDread` and `sceIoIoctl` remain unbound and are recorded as
the two remaining IoFileMgr gaps for the T6 closure audit.

**Next blocker (savedata).** The run stops at
`Missing HLE import sceUtility::sceUtilitySavedataInitStart
(NID_0x50C4CD57)`, the first utility state-machine call.

## 2026-10-30 — T6k: sceUtility savedata SIZES and the main-loop gate

**Symptom.** After T6i the run stopped at
`Missing HLE import sceUtility::sceUtilitySavedataInitStart
(NID_0x50C4CD57)` after 26,058 dispatches.

**Reference review.** Code search found PPSSPP's `sceUtility` and
`SavedataParam` implementation, its save dialog state machine, and psprism /
PCSP savedata stubs. The PSPSDK `SceUtilitySavedataParam` layout was checked
against the live guest structure: the title uses the 0x600-byte layout with
mode 8 (SIZES), game `ULUS10592`, save `GUN9THSYS`, file `SYSTEM`, and the
`msFree`/`msData`/`utilityData` output pointers. The first field dump also
confirmed the SFO title `Gungnir`; no game content was copied.

**Root cause.** No sceUtility dialog state machine. The game asks for save
sizes during boot and polls status until the utility reports completion.

**Fix.** `runtime/hle/hle_utility.*` adds the savedata
InitStart/Update/GetStatus/ShutdownStart state machine. SIZES fills the guest
`msFree`, `msData` and `utilityData` blocks from the virtual memory stick:
free clusters/space from the modeled card, used clusters and KB from the
actual `ms0:/PSP/SAVEDATA/<game><save>` directory, and the utility estimate
from the data and icon sizes. A missing save returns
`SCE_UTILITY_SAVEDATA_ERROR_SIZES_NO_DATA` (0x801103C7) with zeroed usage.
Game/save/file components are validated before touching host paths; only the
0x600-byte layout is accepted so a different version fails closed. Other
savedata modes log once and return a named error rather than fake success;
MsgDialog and GamedataInstall remain unbound counted gaps.

**Evidence.** Red-first `savedata_contracts` failed at "second init rejected
while active" before the handlers were bound. It now covers the SIZES
completion sequence, filled usage blocks, missing-save NO_DATA, bad param
size, path traversal and repeated init. The deterministic 200,000-dispatch
run reaches 2,178 vblanks and 738 flips, 158/215 imports bound and 85
executed, zero missing or silent NIDs, and stops at the dispatch bound
(`0x0899C3DC`). The savedata InitStart/Update/GetStatus/ShutdownStart calls
all executed. A 30-second unpaced run reaches 1,810,000 dispatches and 7,394
flips, i.e. the title is in its main loop. The two-run determinism check now
uses a 200,000-dispatch bound because a wall-clock stop is no longer a
deterministic comparison point.

**Next gate.** There is no missing HLE import at this depth. T6o owns the
named module-init-completion gate, the counted-gap audit and the deepest-run
determinism closure; T7 still owns the first rendered GE frame.

## 2026-10-30 — T6o: module-init gate, determinism and counted gaps

**Symptom.** T6k left no missing HLE import: the 200,000-dispatch run stopped
at the dispatch bound with the title in its main loop. The remaining T6 needs
were evidence, not another handler.

**Gate.** `boot_runner` now reports `module_init_complete`, `stop_cause` and
the kernel thread table. The gate is `flips > 0 && buffer_width != 0 && no
missing import`; a real presented frame proves module_start returned or the
title reached its own loop. The 200,000-dispatch run reports `true` /
`dispatch_limit` with `user_main`, `SOUND thread(sas)`, `at3mem thread` and
`SOUND thread(UMD)` in the table. A 30-second unpaced run reaches 1,810,000
dispatches and 7,394 flips.

**Determinism.** The two-run check now bounds both sides by 200,000
dispatches instead of wall time. The stable comparison includes the gate,
stop cause, thread table, dispatch/vblank/flip counts, import lists and stop
PC; both runs are identical.

**Counted gaps.** 57 imports remain unbound and are recorded in `HLE.md`:
IoFileMgr Dread/Ioctl (2), kernel/ThreadMan/Utils/UMD small calls (8),
sceUtility MsgDialog/GamedataInstall (9), ATRAC track (6), SAS controls (8)
and sceMpeg (24). They are absent handlers, not stubs: if one executes, the
run still stops with its NID. Other savedata modes return a named error and
move to T12/M5 with the first non-SIZES use. No ATRAC track decode, SAS
control or movie call executed at this depth.

**Closure.** T6 is complete: module init and the main loop are evidenced,
the deepest bounded run is deterministic, and every unexecuted counterpart
is counted. T7 remains required for M3 because no GE frame is rendered yet.

## 2026-10-31 — T13a: one session, two hosts

**Symptom.** The Linux boot runner reached module init and the first flip, but
the Android APK still compiled only the GE/input/audio-sink subset. The HLE,
ATRAC, savedata and generated AOT corpus were absent from the APK, so no device
run could share the desktop boot path.

**Falsification.** `cmake/AndroidRuntime.cmake` was checked against the
`gungnir_hle` target: it had no `runtime/hle/*`, `atrac_file.cpp`, savedata,
`elf32`/`iso_image` or `atrac3p`. A second boot implementation on Android would
have drifted immediately.

**Fix.** `runtime/session/game_session.*` now owns the boot flow (ELF load, AOT
registration, HLE registration, bounded run, stop cause and import accounting);
`tests/hle/boot_runner.cpp` is a CLI + JSON shell over it. The Android target
compiles the same session, HLE, ATRAC, savedata and yakumo sources. The app
starts the session on a worker thread, mounts `files/psp` as ms0, writes the
packaged `configs/nids.csv` to `files/runtime/nids.csv`, loads the user's
`game.elf` from `files/game/`, and logs one `session_stop` line. Without a
corpus the APK builds a named `game_corpus_missing` stub instead of running
empty.

**Evidence.** VERIFIED on the host: the refactored `tests.test_boot` suite still
reaches the first `sceDisplaySetFrameBuf`, reports identical two-run traces and
has no executed unbound import (`3 tests OK`, 45 s). VERIFIED on the arm64 phone
(M2102K1AC, Android 14, Adreno): the real-corpus APK logged
`session_stop cause=first_flip dispatches=213 flips=2 ... imports_unbound=51
imports_executed=37 missing_import=0 module_init_complete=true`, and
`scripts/android/run.sh` returned `SESSION_OK`. The 51 unbound imports are the
documented gaps and none executed. Details: `docs/qa/2026-10-31-t13a-runtime.md`.

**Regression.** `tests/test_session.py` (report contract + Android source list +
corpus/memstick wiring), `tests/session/session_report_tests.cpp`, and the
existing `tests/test_boot.py` against the real corpus.

## 2026-11-01 — T13b: title screen on device

**Symptom.** The first Android GE frame was black; the synthetic debug quad
appeared as a small rainbow rectangle in the bottom-left, rotated relative to
the UI overlay. After fixing the scale, the title band rendered but its text
was scrambled, and boot stopped at "Checking storage media".

**Falsification and root causes.**

1. *Scale/rotation.* `recreate_swapchain()` overwrote `Config::width/height`
   with the surface size (3063x1335), so the mesh shader divided 480x272 guest
   pixels by the surface extent; the scissor was also clamped to 480x272. The
   surface reports `currentTransform=ROTATE_90` while the code requested it as
   `preTransform`, so the compositor skipped the rotation. Fix: separate
   `virtual_width/height`, fit the viewport to the guest aspect, scale the
   scissor, request `preTransform=IDENTITY`, and treat `VK_SUBOPTIMAL_KHR` as
   success (Granite's Android behavior). VERIFIED: debug quad fills the fitted
   rect upright; title/logo render at the correct aspect.
2. *Scrambled text.* The sprite path copied the top-left vertex into the
   bottom-left corner but never replaced its Y, so every rectangle drew as a
   triangle. Fix: `bl.y = bottom_right.y`; regression
   `through_sprite_quad`. VERIFIED: storage/legal text is legible.
3. *Boot stall.* `sceUtilitySavedataGetStatus` returned `WRONG_TYPE` after the
   dialog reached NONE; the guest polls it until it sees 0. Fix: keep returning
   0 after a successful init (JPCSP documents this). AUTOLOAD/AUTOSAVE were
   also mapped to the load/save paths. The next stop was an unbound
   `sceUmdCancelWaitDriveStat`; the UMD medium/wait calls are now bound.
4. *Movie skip.* The opening movie calls `sceMpegInit`; MPEG is deferred to
   M5/T12. All 24 imported sceMpeg entry points now return explicit named-gap
   errors instead of stopping the session, and the guest skips the movie.
5. *Starvation.* The skipped-movie worker (`loopHacker`, priority 31)
   busy-polled `sceKernelTryReceiveMsgPipe` and `sceKernelCheckCallback`, so
   `user_main` (priority 32) never ran and never sent the worker's 0xBEEF
   event. `sceKernelCheckCallback` now yields 50 us when no callbacks are
   queued; contract `check_callback_yield_contract`. VERIFIED on the host:
   flips rose from 310 to 476 and frame 450 carries the title key art (32
   draws, 21 textured). Device: storage check, ATLUS logo and title path
   confirmed; the user reports it working.

**Open issue: audio.** The title decodes ATRAC heavily
(`sceAtracDecodeData` 1.18M calls in a 2.5M-dispatch host run). The latest
instrumented device run reports `pulled=670277 underrun=19741 submitted=1273
overruns=0 rejected=0 peak=0 xruns=0`: the mixer received only silent buffers
in that run, so the audible distortion heard on the previous build is not yet
reproduced. HYPOTHESIS: ATRAC track decode/streaming or guest audio pacing,
with underrun stutter as the secondary candidate. Next: capture the actual
PCM, fix the source, then re-check peak/underrun/xrun at the title and menu.

**Regression.** `tests/ge/state_machine_tests.cpp`,
`tests/ge/vulkan_tests.cpp`, `tests/hle/kernel_tests.cpp`,
`tests/test_boot.py`; device evidence in the T13b task and the audio log.

## 2026-11-01 — T13b: audio "garbled" and the 16 s device stop

**Symptom.** The title music was choppy and pitch-wrong ("came through a bit,
not unrecognizable"), and device sessions ended about 16 s after launch with
`session_stop cause=guest_stop`, before the title. The mixer reported
`peak=0` for those runs, which hid the cause: the game only starts its music
channel around the 16 s mark.

**Root cause.** `MixerSink` kept one FIFO for every `sceAudio` channel: a
submitted buffer was appended behind the previous one instead of being summed.
The title feeds two channels at once (channel 4 silent, channel 5 music) at
44.1 kHz each, so the FIFO gained about 59 k frames per second. After roughly
a second of overlap the ring filled and `submit()` returned false; the HLE
turns that into `audio sink rejected a buffer` and stops the session. The
serialized channels are audible as music alternating with silence — garbled
but recognizable.

**Fix.** `MixerSink` is now a timeline mixer: each channel owns a cursor, a
submitted buffer is summed into a 32-bit ring at that cursor, and the device
pull clamps once. Channels start 2048 frames (46 ms) ahead of the read
cursor. A channel more than 64 frames behind restarts ahead of the device
(`resyncs`), and a timeline that outgrows the ring restarts from empty
(`overruns`); neither rejects the submission or stops the guest. `peak` now
describes the mixed output. Volume, mono duplication and the final clamp are
unchanged.

**Evidence.** `tests/audio/mixer_tests.cpp` adds the two-channel sum, overflow
restart and late-channel resync contracts. Against the HEAD (FIFO) mixer the
new contract fails for exactly those two reasons (`two channels are summed`,
`overflow does not reject the submission`); with the timeline mixer all four
groups pass. Device run after the fix: no `audio sink rejected`, the session
continues past the title, and `peak` is nonzero once music starts.

**Counted gaps.** `underrun_frames` can now include silent timeline gaps
(a zeroed ring entry is indistinguishable from written silence once the write
head has passed it); the device's AAudio xrun count remains the authoritative
stall signal.

## 2026-11-01 — T13b: title stalls on descriptor-pool exhaustion

**Symptom.** About 16 s after launch the screen froze on the last frame and
the log filled with `render_failed: texture descriptor failed
(-1000069000)` (`VK_ERROR_OUT_OF_POOL_MEMORY`) plus repeated
`lifecycle_resumed`; the process was alive but could not draw again.

**Root cause.** The GE backend created its texture descriptor pool with 64
sets and without `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT`, so the
`vkFreeDescriptorSets` in `destroy_texture` was invalid usage and never
returned capacity. The title uploads more than 63 distinct textures (plus the
white fallback), and the first allocation past the limit failed.

**Fix.** The pool now has 4096 sets and the free flag: destroyed textures
return their sets. `ge_vulkan_tests.many_textures` uploads 96 distinct
textures in one headless backend and renders with each. Against the old pool
it fails at texture 63; with the fix the test passes.

**Follow-up.** The render loop keeps trying to recreate the surface when a
draw fails, so a persistent resource error freezes the picture without
exiting. After three consecutive failures it should stop with the named error;
this is logged for the next cleanup pass.

## 2026-11-01 — T13b: title music stalled at the end of the first 128 KiB

**Symptom.** With the mixer fixed the title music played, then the picture
froze at about frame 420 while the session stayed alive. The session thread
held 100% CPU; `[session] heartbeat` showed `SOUND thread(UMD)` (priority 16)
running at `0x0883AAC0` and `user_main` (priority 32) merely ready. Dispatches
ran at ~6 M/s while virtual time advanced only 0.6 s per 2 s.

**Root cause.** `SOUND thread(UMD)` calls `sceAtracDecodeData` in a tight
loop. Our handler returned immediately on both success and
`SCE_ATRAC_BUFFER_IS_EMPTY`, so the priority-16 thread never yielded. The
track was opened with `sceAtracSetData` and a 128 KiB buffer (`data_size`
959016, `data_available` 130976 = 176 frames); after those frames the decode
returned BUFFER_IS_EMPTY forever and starved the thread that would have fed
more data. Real hardware spends media-engine time per decode attempt, which
is the scheduling point.

**Fix.** `sceAtracDecodeData` now waits for the media engine on every attempt,
including an empty buffer: `1815 + bytes * 158 / 100` µs for stereo ATRAC3+
and `970 + bytes * 44 / 100` for stereo ATRAC3 (the PPSSPP hardware fit).
After the fix the thread table shows `user_main` running and all sound threads
waiting on audio/delay, frames advance past the title, and the mixer reports
increasing `peak` with no underruns after startup.

**Regression.** `an empty decode still accounts for the media-engine frame
time` (and the successful-decode variant) in `tests/hle/kernel_tests.cpp`;
both fail when the delay is removed.

**Next gap.** The title menu and difficulty screen work (`NEW GAME` ->
`BASIC`/`ADVANCE`). Confirming a difficulty faults in guest code at
`0x088FFF64` (`Guest memory access outside PSP RAM/EDRAM at 0x20`, a0=0): a
getter at `0x088040A4` returns a null object and the caller dereferences
`this+0x20`. That belongs to the new-game initialisation path.

## 2026-11-01 — T13i: new-game confirm faults in guest initialisation

**Reproduction.** The boot runner can now drive menu paths on the host with
`--pad-press FLIP[:BUTTON[:HOLD]]` (a display-flip observer publishes the
mask). Cross at flip 460 opens the new-game path; confirming BASIC at 720
faults at `0x0880A8F8` (`Guest memory access outside PSP RAM/EDRAM at 0x0`,
a0=0). Selecting ADVANCE first stops at the unbound
`sceUtility::sceUtilityMsgDialogInitStart` instead.

**BASIC path.** The caller at `0x0880A8F0` asks the guest allocator
(`0x08985BE8`) for 104 bytes. The allocator returns 0, the caller branches to
`0x0880A914` and dereferences the null pointer at `gpr17+0`. Allocations
normally land in the guest `UserSbrk` block (`0x08B38000`, 0x19000 bytes);
`PSPRECOMP_TRACE_PC=0x0880A8F8` shows ~1,900 successful returns before the
single zero, so the block is not simply exhausted. The guest prints
`POLL SEMA` immediately before the failure. Our `sceKernelTryAllocateFpl`
contract matches PPSSPP (success writes the pointer and returns 0, failure
returns `NO_MEMORY`), so the failing path is the game's own heap/free list.

**Savedata.** The boot calls savedata mode 11 (LIST) for the SYSTEM, STORY*
and CLEAR* save names; the handler still returns `0x80110300`. The game's
save-slot list is therefore uninitialised before the new-game path, which is
the leading candidate for the missing object.

**References.** PPSSPP `sceKernelMemory.cpp` (FPL contract), `sceUtility.cpp`
and `PSPMsgDialog.cpp`/`PSPDialog.cpp` (dialog state machine); the
`snuri00/psp-web-recomp` message-dialog HLE (headless auto-accept writes
result 0 and `buttonPressed = YES`); JPCSP `sceUtility.java`; PSPSDK
`psputility_msgdialog.h`/`psputility_savedata.h` (LIST = 11).

**Next.** Implement the four message-dialog imports and savedata LIST, then
rerun both pad scripts. Both are already named gaps.

## 2026-11-01 — T13i: message-dialog HLE and savedata LIST

**Change.** The four `sceUtilityMsgDialog*` imports now run the PSP status
machine with a documented headless auto-accept: once visible the dialog
completes with result 0 and `buttonPressed = 1` (YES), the V1/V2/V3 parameter
sizes are accepted and anything else fails closed. Savedata mode 11 (LIST)
fills `SceUtilitySavedataIdListInfo` from the save directories matching
`gameName + saveName`, honours `*`/`?` wildcards, sorts entries, strips the
game prefix and reports `0x80110328` for a missing or invalid `idList`.

**Evidence.** Red-first `utility_dialog_contracts` and the LIST block failed
before the handlers were bound (missing imports / named mode-11 gap). They
now cover the status sequence, the auto-accept result, V1/V2/V3 sizing,
exact/wildcard/limited/empty lists and the rejected `idList`.

**ADVANCE path.** With `--pad-press 460:cross:12 --pad-press 700:down:8
--pad-press 720:cross:12` the run no longer stops at
`sceUtilityMsgDialogInitStart`; it reaches 2,000,000 dispatches, 9,199 flips
and the dispatch limit at `0x08805DEC`. The game now calls savedata mode 4
(LISTLOAD) 613 times: `sceUtilitySavedataInitStart` and `SavedataGetStatus`
dominate the tail histogram, i.e. the title is retrying a UI-less load menu.
LISTLOAD is the next named gap.

**BASIC path.** Confirming BASIC (`--pad-press 460:cross:12 --pad-press
720:cross:12`) still faults at `0x0880A8F8` with the same 104-byte allocator
request returning 0. The dialog gap was not the cause of that failure.

**References.** The same PPSSPP/JPCSP/PSPSDK set as above, plus PSPSDK
`psputility_savedata.h` for `SceUtilitySavedataIdListInfo`/`Entry` and
PPSSPP `SavedataParam::GetList` for the wildcard search string.

### LISTLOAD research (mode 4)

The title passes `saveNameList = GUN9THSTORY00..31`, `idList` non-null but
unused, `fileName = STORY` and `focus = 3` (LATEST). Reference semantics:

- **Candidate list.** PPSSPP `SavedataParam::SetPspParam` reads the
  zero-terminated 20-byte `saveNameList` and keeps only names whose save
  directory exists (`listEmptyFile = false` for LISTLOAD/LISTDELETE); a
  `"<>"` entry expands to every directory with the game prefix. Without a
  usable list it falls back to the single `gameName + saveName`. JPCSP's
  `MODE_LISTLOAD` does the same over `saveNameList` with `isPresent()`.
- **No candidates.** PPSSPP `PSPSaveDialog` shows `DS_LOAD_NODATA`; the only
  action is cancel, which sets `common.result =
  SCE_UTILITY_SAVEDATA_ERROR_LOAD_NO_DATA` (0x80110307) and finishes.
  JPCSP uses the same code (`ERROR_SAVEDATA_LOAD_NO_DATA`).
- **Selection.** `focus` maps to `SavedataParam::GetFirstListSave` /
  `GetLastListSave` / `GetLatestSave` / `GetOldestSave` / `GetFirstDataSave`
  / `GetLastDataSave`; unknown and FIRSTLIST select index 0. LATEST/OLDEST
  compare the save's `PARAM.SFO` mtime and skip zero-size entries.
- **Load.** The selected name is used as `saveName`; PPSSPP's
  `LoadSaveData` writes it back to the request (when `fileName` is
  non-empty), then the normal LOAD path sets `bind = 1021`, `dataSize`,
  SFO fields and icons. Missing directory → 0x80110307; missing data file →
  0x80110309; broken SFO → 0x80110306.
- **PSPSDK sample** (`src/samples/savedata/utility/main.c`) drives the same
  status loop: VISIBLE → Update, QUIT → ShutdownStart, FINISHED/NONE → done,
  with `saveNameList` populated before `sceUtilitySavedataInitStart`.

Headless policy to implement: empty list auto-cancels with 0x80110307;
a non-empty list auto-selects the `focus` entry and performs the LOAD, with
the selected name written back. Recorded here before implementation.

### LISTLOAD implemented and the next fault

Mode 4 now follows the reference policy: candidates from the zero-terminated
`saveNameList` (existing directories only, `"<>"` expansion, single-save
fallback), `focus` selection, `0x80110307` for an empty list, selected name
written back, then the normal LOAD path. Red-first
`savedata_listload_contracts` covers first/last/latest/oldest selection,
missing-entry skipping, `"<>"`, the single-name fallback and the empty list;
it now passes.

Host ADVANCE (`--pad-press 460:cross:12 --pad-press 700:down:8 --pad-press
720:cross:12`) no longer retries mode 4. It passes the save probe and reaches
the new-game setup, then faults on an indirect call whose target is 0
(reported at dispatch `0x08806034`; the call at `0x0880602C` sets
`ctx.pc = gpr6`, and `gpr6 = [gpr7 + 4]` is zero). BASIC still faults at
`0x0880A8F8` when a 104-byte allocator request returns 0. Neither path has an
unbound HLE import; both look like the same missing or null guest object.

### Null-object trace (2026-11-01)

**BASIC allocator.** With an allocation-site diagnostic the 104-byte request
fails deterministically at call 1969 on a fresh memory stick, `self` is
`0x08B38D20`, and the heap descriptor is `0x08ACF3A8`. The same 112-byte
chunk is handed out about 1,006 times before that, and the last successful
chunks reach `0x08B40030` inside the 0x19000 `UserSbrk` block
(`0x08B38000`..`0x08B51000`), i.e. roughly 69 KB is still unused when the
allocator gives up. The call chain is guest code
`0x08985BE8` -> `0x08973094` -> `0x08977F4C`; no HLE allocator is involved.
This points at corruption or fragmentation of the game's own free list, not
at an HLE allocation contract mismatch.

**ADVANCE null read.** A gdb catch on C++ throws identified the fatal path
as `GuestMemory::load8` <- `GuestMemory::aot_load32_slow` <-
`recomp_unit_0101_entry`, i.e. translated code at guest `0x0899B3F0`. That
routine copies four 4x4 VFPU matrices from a source pointer and the caller
passes `a1 = 0`. There is no missing import and no HLE handler in the throw.
The savedata trace shows no request after LISTLOAD returns `NO_DATA`; the
last I/O before the fault is the story `data.dar` reads and an async open of
`bgm/bgm01.at3`. The fault is therefore guest state (a null matrix source),
not an unbound HLE call.

### HLE call ring

`GUNGNIR_HLE_RING=N` now records the last N HLE import calls (`a0`-`a3`,
`v0`/`v1`, return PC, thread) and the session prints them when a run stops on
a fault. In the ADVANCE window the calls immediately before the matrix-copy
fault are ordinary: `sceRtcGetCurrentTick`, four
`sceKernelDcacheWritebackRange` calls with size 0, `sceKernelCheckCallback`
(returns 0, no queued callback) and `sceAudioOutputPannedBlocking` (channel
4, returns 0). In BASIC the allocator's
`sceKernelCpuSuspendIntr`/`ResumeIntr` bracket the heap and the last call is
again `sceAudioOutputPannedBlocking` before the failed 104-byte request. No
HLE return looks wrong, so the divergence is in guest state rather than a
bad import result.

### Reference research for the shared null-object fault (2026-11-01)

**Diagnostics inventory.** `PSPRECOMP_TRACE_ON_ERROR=1` already keeps a
64-entry ring of *outer dispatches* (pc + a0-a3 + sp + ra) and prints it on an
error. The chained-call hooks (`set_runtime_pre/post_chained_call_hook`) fire
for dynamic `Runtime::invoke_chained_call` targets only; the compile-time
`Runtime::invoke_chained_direct<...>` template bypasses them, so a guest call
ring built on the hooks alone misses direct calls. `GUNGNIR_HLE_RING` covers
imports only. The ADVANCE fault happens deep inside one outer dispatch, which
is why the existing rings show only ordinary calls before it.

**Call site.** `0x0880602C` is a descriptor dispatch: it loads
`desc = [object + 248]`, adds the 16-bit `this` adjustment at `desc + 0`, and
calls `[desc + 4]`. The descriptor array is static: 116 eight-byte
`{u32 offset, u32 fn}` entries at guest `0x08AD0B7C`. The descriptor whose
function is `0x08838A44` sits at `0x08AD0DE0` (offset 0). `0x08838A44` is a
forwarding thunk: `copy(dest = a1, src = a2)` through `0x0899B3F0`, which
loads four VFPU vectors from `src` and writes them to `dest`. The faulting
argument is `src = 0`, not the descriptor. The preceding `0x08838A40` is the
delay slot of the previous `jr $ra`; the analyzer's function seed there is a
false entry (harmless here, but it must not be read as the thunk's prologue).

**Basic allocator.** `0x0880A8F0` requests 104 bytes from the game allocator
at `0x08985BE8` and then constructs the result at `0x088104F8`; the
constructor stores the vtable `0x08AD0308` at `object + 100`, so this is a
C++ `new` of a 104-byte class instance. The allocator is the game's own
(`faile[%s]:%dbyte Allocation(rest[%d])` is its diagnostic, not a public
allocator), and the failing call is not sbrk exhaustion. Immediately before
it, the ring shows a successful 112-byte request (`CpuSuspendIntr`
`a1 = 0x70`), and the allocator state at `0x08ACF3A8` holds small-bin heads
`0x08ACF58C` / `0x08ACF5E4` / `0x08ACF63C`, 0x58 bytes apart. HYPOTHESIS: a
size-class/free-list bookkeeping or chunk-header corruption, not a missing HLE
allocator contract.

**Reference behaviour.** PPSSPP runs Gungnir past new game (its two open
reports are texture sampling at `#15449` and an old battle crash at `#4446`);
`sceKernelCpuSuspendIntr`/`ResumeIntr` there return/consume the previous
interrupt state and test bit 0 on resume, which matches our handler, so the
allocator's critical section is not obviously broken by the HLE. No
decompilation or symbol source exists for the title.

**Next instrumentation (proposal).**
1. A dynamic chained-call ring on the pre/post hooks (target, depth, a0-a3),
   filtered around `0x08838A44`, to identify its caller and the null `src`
   argument. The hooks already fire for `invoke_chained_call`.
2. A dispatch-boundary guard that snapshots the allocator header region
   (`0x08ACF3A8`..`0x08ACF700`) before and after each outer dispatch and
   reports the first dispatch that changes it without going through the
   allocator, giving a guest PC for the corrupting write.

### Root cause: blocking audio clobbered the resumed thread's registers

The chained-call ring (`GUNGNIR_CHAIN_RING`) showed the null matrix source was
passed down a dynamic chain: `0x08834804` receives `a1 = 0` and forwards it as
the wrapper's `src` to `0x0899B3F0`. The descriptor table and wrapper were
sound; the argument was already zero. The ring then showed the switch to
`user_main` happening inside `sceAudioOutputPannedBlocking`, and a dispatch
trace (`PSPRECOMP_TRACE_PC=0x08806034`) showed the resumed context had
`v0 = 0` even though the saved context held the correct `0x08B38730`.

`hle_audio.cpp` evaluated `kernel().finish(ctx, enqueue_blocking(...))`.
`enqueue_blocking` parks the caller in `Kernel::block` and may switch to
another thread; `kernel().finish(ctx, ...)` then wrote the audio result into
the **new** context. That zeroed the resumed thread's `v0`, so the engine
continued with a null matrix pointer. The fix captures the execution-context
token before the blocking call and only finishes when the same thread is still
current; the parked thread's result is already recorded by `block`.

Red-first contract: `audio_contracts` sets a sentinel in the parked loader's
`v0`, then blocks another thread in `sceAudioOutputPannedBlocking`; the old
code failed "blocking output must not clobber the resumed thread's result",
the fix passes it. PPSSPP's `sceAudioOutputPannedBlocking` also returns the
result to the parked thread when it resumes, not to whichever thread the
scheduler switched to.

With the fix, the ADVANCE pad script no longer faults: it reaches the
dispatch limit at `0x08805DEC` with 6,965 flips instead of dying on the null
matrix at `0x0899B3F0`. The BASIC pad script also runs past the former
1969th-allocation failure: 1,500,000 dispatches, 6,665 flips, dispatch limit
at `0x0883AAC0`. Both new-game paths now survive the transition instead of
dying in guest code.

### Native-heap growth after the new-game transition (T13j)

**Symptom.** After the ADVANCE confirm the session reaches the "Dept. Heaven
Episode" prologue with no guest fault, then Android kills the process:
`dumpsys meminfo` reports `Native Heap Pss Total 8022869 KB` (heap size
8.5 GB) after ~1140 guest frames (20 fps), and `dumpsys activity exit-info`
records `reason=3 (LOW_MEMORY)`. Guest RAM is 32 MiB, so the growth is host
retention, not guest memory.

**Falsified causes.** The session heap is not the frame queue (`FrameQueue`
holds only the latest frame and drops the rest, counted) and not the GE list
map (`ge_lists` drops finished lists). The descriptor pool exhaustion from
T13b is separate: the pool is 4096 sets with the free flag and no
`VK_ERROR_OUT_OF_POOL_MEMORY` appears in the run.

**Root cause.** `StateMachine::resolve_texture` appended every decoded texture
to `cpu_textures_`, which only `StateMachine::reset()` cleared; production
never calls `reset()`, so one raw guest-texture buffer plus its `Texture`
object stayed alive per textured draw forever. `TextureCache` (decoded RGBA
pixels keyed by content hash) was also unbounded, and the Vulkan
`Impl::textures` map kept one `VkImage` per content key for the process
lifetime. Animated/CLUT-swapped content changes the content hash, so all three
grew every frame: 79 draws/frame and ~6.6 MB/frame matches the measured
8 GB in 30 s.

**Reference behaviour.** PPSSPP keeps `lastFrame` and
`EstimateTexMemoryUsage` per texture and runs `TextureCacheCommon::Decimate`
on a 13-frame cadence: above 128 MiB of estimated host memory it drops
entries unused for 200 flips (6 for CLUT variants, issue #10418), with a
separate 32 MiB/100-flip second cache
(`GPU/Common/TextureCacheCommon.cpp`, `TEXCACHE_MIN_PRESSURE`,
`TEXTURE_KILL_AGE`). psprecore evicts GPU textures after
`frame_number - last_frame > 240` and defers destruction into the frame-slot
garbage list (`runtime/src/platform/gpu_vulkan.cpp`, `evict_textures`). Both
use an age/size limit rather than process-lifetime retention.

**Fix (bounded, ownership-correct).**
- `DrawState` carries `shared_ptr` owners for the decoded image and the source
  texture, so a frame queued for presentation keeps exactly what it samples;
  the cache only owns its own entries.
- `TextureCache` is an LRU with a 64 MiB default byte budget; eviction drops
  only the cache reference, never an in-flight draw's data.
- The Vulkan texture map is LRU-bounded by count (512) and bytes (128 MiB).
  Eviction runs after the frame fence (or readback) at frame end, and the
  destroyed images move to a retired list reaped at the next idle frame, so a
  descriptor bound by the current frame is never freed underneath the GPU.
- `invalidate_texture` also defers destruction instead of waiting the device
  idle on the guest thread.

**Regression.** `tests/ge/state_machine_tests.cpp::bounded_texture_retention`
drives 400 unique 256x256 textures and checks the retained entry count and
bytes stay inside the budget while an early in-flight draw can still decode
its source texture. It was red against the unbounded cache (399 entries,
~100 MB) and is green with the budget. `tests/ge/vulkan_tests.cpp::
bounded_gpu_texture_cache` checks the GPU limit and byte budget across 32
distinct uploads. The state-machine binary is now wired into
`tests/test_ge.py`, which previously built but never ran it.

**Device verification.** APK `ce3a32a5c416f…` (debug, arm64-v8a) on the
M2102K1AC: `qa-newgame.sh` reached `NEW_GAME_REACHED` with native-heap PSS
180-182 MB for the first ~23 s, a 168-235 MB band during the prologue scene
change (79 -> 272 draws/frame), then 190 MB at 60 s and 1,620 guest frames.
The pre-fix run reached 8.0 GB at ~1,140 frames and was killed; the same
script now exits 0 instead of `PROCESS_MEMORY_GROWTH`/`PROCESS_KILLED`.

### "Hang" after the prologue: an unbound `sceKernelGetGPI` stop (T13i)

**Symptom.** After the native-heap fix the device run reached the "Dept.
Heaven Episode" prologue, then froze on a black screen with only the touch
overlay: no guest fault, no process kill, `guest_frame=` lines stopped and the
process slept at ~400 MB RSS. The screen looked hung, but the session had
ended.

**Root cause.** `logcat` ended with
`session_stop cause=missing_import ... stop_pc=0x089A22F4` and
`session_stop_reason=Missing HLE import UtilsForUser::sceKernelGetGPI
(NID_0x37FB5C42)`. `configs/nids.csv` already carried the NID, but no handler
was registered, so the import trapped and stopped the session. The app keeps
presenting its last frame (black at that point), which reads as a hang rather
than a named stop.

**Reference behaviour.** PPSSPP `Core/HLE/sceKernel.cpp` returns the
dev-kit GPI switch bits and notes they are always 0 on production systems
(`sceKernelSetGPO` only stores the debug LED mask; issue #19132 covers the
dev-kit-only GPO feature). Retail Gungnir therefore expects 0.

**Fix.** `sceKernelGetGPI` joins the other UtilsForUser no-ops and returns 0;
`sceKernelSetGPO` keeps returning 0. Red-first `utils_contracts` asserts the
call returns 0 **and** that the runtime did not stop, because an unbound
import only sets the stop reason and leaves `v0` untouched — a value-only
assertion passed against the unbound import.

**Visibility.** `main.cpp` now logs one `session_unbound=<library>:<nid>` line
per import still unbound when the session stops, so the remaining gaps for
this path are visible in a single run instead of one stop at a time.

**Device verification.** APK `9173b120ee5f…` (debug, arm64-v8a, local corpus)
on the M2102K1AC: `qa-newgame.sh` reached `NEW_GAME_REACHED
native_heap_pss_mb=194`, and the session kept running instead of stopping. The
prologue plays through the "15 years" interstitial into the first event scene
(`Eastern Wilds`, `Giulio: This rain's getting worse...`), and pad taps advance
the dialogue to the next speaker (`Young Soldier: Onions, red`). 3,420 guest
frames with no `session_stop`, native heap 180-190 MB. Evidence:
`build/qa/android/t13i-newgame-20261101-000529/` (gitignored).

**Remaining unbound imports on this title.** After binding GPI the session still
counts 10 unbound imports out of 215; none has executed yet while the prologue
plays. The count is derived from the intake import table plus
`configs/nids.csv`, not from a device stop:

`IoFileMgrForUser` `sceIoIoctl` (0x63632449), `sceIoDread` (0xE3EB004C);
`Kernel_Library` `sceKernelMemcpy` (0x1839852A); `ThreadManForUser`
`sceKernelWaitEventFlagCB` (0x328C546A), `sceKernelReferThreadProfiler`
(0x64D4540E); `sceUtility` `sceUtilityGamedataInstallInitStart` (0x24AC31EB),
`GetStatus` (0xB57E95D9), `Update` (0x4AECD179), `ShutdownStart`
(0x32E32DCB), `Abort` (0x180F7B62).

Any of these can end the session with a named `missing_import` stop; the ten
are the known gap set for the next device passes, not silent zero-returns.

### Host boot runner grew to 19 GB without a frame sink (2026-11-01)

**Symptom.** Driving the prologue through `build/profile-test/boot_runner`
(no `--frame-json`) ballooned to 19 GB RSS within a minute and died with
`session_run_failed: std::bad_alloc` under an 8 GiB address-space cap.

**Root cause.** `sceDisplaySetFrameBuf` handed `pending_draws` to the frame
sink only when one was installed, so with no sink every translated draw (and
the texture references it owns) was retained for the process lifetime. The
APK always installs a sink, which is why the device heap stayed at 180-190 MB;
the Linux boot runner does not, so the same code leaked only on the host.

**Fix.** The flip swaps the pending vector out unconditionally and only then
offers it to the sink, so draws are delivered when something consumes them and
dropped when nothing does.

**Regression.** Red-first `draw_retention_contracts` in
`tests/hle/media_tests.cpp` submits a real through-mode triangle list, flips
without a sink and requires `ge_pending_draw_count() == 0` (it reported the
retained draw before the fix); it then installs a sink and checks the draw is
delivered and cleared. `ge_pending_draw_count()` is the observer.

### Harness load skews frame pacing and can look like a stall (2026-11-01)

**Observation.** The battle intro movie played "super slow" in a run where the
driver polled `adb logcat -d` (full buffer) and `dumpsys meminfo` every two
seconds, and it played smoothly in a later run of the same build with a lighter
driver. The guest, the HLE and the APK were identical.

**Consequence.** Frame rate and "it looks stuck" observations from a script
that shells out to `logcat`/`dumpsys` every couple of seconds are not evidence
about guest performance. `qa-newgame.sh` currently samples meminfo every two
seconds during the monitor window and greps `logcat -d` in the same loop.

**Rule.** Drive input from a quiet loop: stream `adb logcat -s gungnir:I` to a
file once and grep that file, sample `dumpsys meminfo` at >=10 s intervals, and
never treat a frozen screenshot from a polling run as a hang until
`guest_stall`/`session_stop` confirms it.

### Pre-battle stall: the frame thread parks in blocking audio (2026-11-01)

**Symptom.** After the battle intro movie and the "Ace deployed!" lines the
screen stops updating, but the process keeps dispatching: the app is at ~13% of
one core, `guest_frame=` stops, and neither the dispatch limit nor the wall
clock ever fires.

**Diagnosis (new watchdog).** `session.stall_seconds` (Android default 30) runs
a frame watchdog: if no guest frame reaches the display for that long, it logs
one line and requests a stop, which the guest honours at its next heartbeat.

```text
guest_stall seconds=30 flips=2484 inflight=sceAudio::sceAudioOutputPannedBlocking last_import=sceSasCore::__sceSasCore
session_stop cause=host_stop dispatches=950000 flips=2484 ... stop_pc=0x089A25EC
```

`stop_pc` is in the HLE thunk range, and the parked import is the blocking
output. `inflight` is process-global, so read it as "the last thread that
entered an import is parked there": the other guest threads keep dispatching,
which is why the kernel's scheduler-deadlock check never sees an idle system.

**Hypothesis (not yet proven).** The frame-producing thread is parked in
`sceAudioOutputPannedBlocking` and is not woken at its drain deadline, or is
woken but the guest immediately re-blocks; the remaining threads keep running a
wait loop, so only the picture freezes. The audio wait is the only place the
guest parks with `WaitType::Audio`, and its wake-up runs in
`Kernel::process_timers` via `host_poll`, which retries the enqueue.

**Candidate mechanisms (to test, none proven).**
1. `g_channels[i].waiting` left set after a blocking call that never completes:
   the channel then stays busy and every later blocking output parks again.
   PPSSPP had exactly this class of bug — `AudioChannel::DoState` carries a
   compatibility fix for a save that stored `waitingThread = -1` ("a failed
   blocking wait left -1 here to keep the channel busy for good").
2. `drain_us()` returns `uint32_t` from a 64-bit division, so a very large
   `sample_count` truncates the deadline; a bogus data length would park the
   caller for minutes.
3. The deadline is observed (the runtime's starvation hook advances the clock
   and calls `process_timers`), so "no scheduler tick" is ruled out.

**Next step.** Reproduce from a snapshot at the battle (T15) and inspect the
audio channel state at the stall, instead of replaying ten minutes per attempt.

**Harness caveat.** See the pacing note above: the earlier "super slow" battle
movie was measured while the driver polled `logcat`/`dumpsys` every two
seconds; the same build played it smoothly in a quieter run.

## 2026-11-01 — T15d: snapshot surfaces (host CLI + Android menu) land

**Implemented (VERIFIED on host unit tests, real-corpus evidence pending in
T15d acceptance).** `GameSession::save_snapshot/load_snapshot/request_resume`
(`runtime/session/game_session.*`): corpus hash binds the guest ELF plus a
toolchain tag, config hash binds `load_base` + `boot_path`; save flushes the
live CPU into the current thread; load restores RAM/VRAM/CPU/clock/kernel/HLE,
resets the mixer sink (logged `mixer_reset`), and `run()` resumes from the
restored PC (idle stub when no thread is current). A failed load poisons the
session (`session_load_failed` on the next `run()`); byte-level rejections
leave it untouched.

`boot_runner --snapshot-out/--snapshot-in` (save after the run, load before
it; `CountingSink::reset` so post-restore audio stats are clean). Android:
Save/Load rows in the Menu overlay (`SettingsOverlayView`) through
`nativeSaveSnapshot/nativeLoadSnapshot` into
`getExternalFilesDir()/snapshots/snapshot.gsnap`, stop+join+save/resume or
stop+join+load+restart under `g_snapshot_mutex`, plus a `snapshot_in` intent
extra. `AudioSink::reset()` (default no-op; `MixerSink` overrides) is the
named mixer reset point. Format, tag table (23 sections), failure list and
the adb flow are in `docs/SNAPSHOT.md`.

**Incidental fix (VERIFIED by red test + real-game `test_boot` green).**
The blocking-audio HLE wrapper finished with the enqueue result even after
parking: on a same-thread park+resume with no other-thread switch the wake
result in v0 was clobbered with 0. It now skips the finish when the enqueue
parked (0 return = parked, wake owns v0).

## 2026-11-01 — T15d evidence: restore-vs-continuous divergence (HYPOTHESIS)

**Status: not proven.** Save/load round trips cleanly at the first battle
(ADVANCE pad script, 1.5M dispatches, 35.9 MB snapshot) and restored runs
complete cleanly with valid thread/waiter structure and the same audio
deadlines. Restore is deterministic across independent restores; fresh runs
are deterministic across boots. But restored runs diverge slightly from
continuous runs: identical flips/vblanks, ~2 ms virtual gap, shifted
scheduler phase, a few hundred scattered bytes growing with window
(30 B at N=1 → ~330 B at 200k dispatches; N=1 boundary exact for live state).

**Falsified:** host wall-clock nondeterminism (fresh runs minutes apart
agree; independent restores of one file agree), pad-observer asymmetry
(restore with pads == restore without), dispatch-count misalignment
(rechecked with aligned totals), mid-unit stop (loop stops on exact outer-
dispatch boundaries; N=1 matches).

**Leading hypothesis (unproven):** a guest-visible host timestamp cell
(3–4 bytes just past `image_end`, plus two 37 B heap records) is read by
guest code early post-resume; restored runs start with a stale cell and take
a suspend-age-like branch. Consistent with everything, including C-vs-D
agreement. Next: identify the reader (candidate: savedata/ATRAC streaming
timeout logic) or run a tight save→restore loop with a fresh cell.

Per project rules this stays HYPOTHESIS until T15e manual QA signs off;
nothing about the feature counts as proven before then.

## 2026-11-01 — T15d divergence root-caused: guest wall-clock base not snapshotted (FIXED)

**Symptom.** Restored runs diverged slightly from continuous runs: same flips
and vblanks, ~2 ms virtual gap, shifted scheduler phase, a few hundred
scattered bytes growing with window (30 B at N=1 → ~330 B at 200k dispatches).

**Root cause (VERIFIED).** The differing 8-byte cell just past `image_end`
decoded as two Unix-microsecond timestamps minutes apart — the exact wall
times of the two runs. Source: `guest_unix_us() = boot_unix_us() +
kernel().now_us()`, where `boot_unix_us()` was a per-process static. Fresh
runs agree behaviorally (same boot base within a run; only stored tick values
differ), but a restore in a new process mixes pre-save ticks (old base) with
post-restore ticks (new base): the guest sees a minutes-scale time jump and
branches differently. N=1 boundary was exact because the single dispatch had
not yet read the clock.

**Fix (VERIFIED by red-first tests + mutation checks).** `ClockState` gains
`wall_base_us`, the clock section grows 24 → 32 B (24 B legacy files still
load and keep the current base), `GameSession::save_snapshot` stores
`hle::wall_base_us()` and `load_snapshot` restores it. Mirrors PPSSPP's
`rtcBaseTime`/`rtcBaseTicks` + `__RtcDoState` (`Core/HLE/sceRtc.cpp`:
"a base time that everything is relative to ... so time doesn't move
strangely with savestates"), behavioral reference only — see
docs/SOURCE_PROVENANCE.md. Real-corpus restore-vs-continuous equivalence
re-run is the remaining T15d evidence.

## 2026-11-01 — T15d divergence root cause #2: starvation-boundary phase (FIXED)

**Follow-up to the wall-base fix.** Preserving `wall_base_us` kept guest
ticks continuous (verified: identical base across restore, virtual time
advancing) but the restore-vs-continuous gap persisted with the same
signature (same flips/vblanks, ~2 ms virtual gap, shifted scheduler phase).

**Root cause (VERIFIED by code inspection + pending real-corpus equivalence).**
Preemption boundaries fire in `Runtime::account_dispatch_work` every
`GUNGNIR_STARVATION_INTERVAL` (20,000) dispatches via
`dispatches_since_import_`, which lives in Runtime and was never
snapshotted. A fresh process restarts the counter at 0 while the continuous
run holds phase `c`, so every future boundary — and the thread interleaving
each boundary selects — shifts by `c` dispatches and never reconverges
(excess is preserved modulo the interval). Fits all evidence: N=1 exact (no
boundary crossed in one dispatch), onset within the first 50k window,
vblank/flip-driven state identical, thread phase shifted.

**Fix (red-first + mutation-checked).** Yakumo local patch #8
(`runtime.hpp`: `dispatch_phase()`/`set_dispatch_phase()`, no behavior
change, recorded in `third_party/yakumo/PROVENANCE.md`); `ClockState` gains
`dispatch_phase`, clock section 40 B (24/32 B legacy files still load and
default absent fields to 0, matching old behavior); save stores
`runtime->dispatch_phase()`, load restores it. `g_runtime_thread_uid` needs
no snapshot: match/mismatch outcomes of the execution-context check are
identical with a stale uid because the hook updates the global before the
comparison in both runs.

## 2026-11-01 — T15d equivalence proven (real corpus, first battle)

**Result: PROVEN on host.** With both fixes (wall base + dispatch phase),
restore-vs-continuous is behaviorally identical and restores are deterministic:

- `battleP1.gsnap` (fresh 1.5M, ADVANCE pad script, 35.9 MB, 32→40 B clock)
  → restore + 200k (`battleP2.gsnap`) vs fresh 1.7M continuous
  (`battleC2.gsnap`): `dispatches` differ only by construction (200k vs
  1.7M); `vblanks` 23702/23702, `virtual_us` 395422239/395422239, `flips`
  7915/7915, `stop_pc` 0x0883AAC0 both, full thread table (incl. audio/delay
  deadlines) identical.
- Two independent restores of `battleP1.gsnap` in different host processes
  (`battleP2` vs `battleP2b`): **byte-identical files**, identical reports.
- Residual P2-vs-C2 delta is 44/35850059 bytes in 8 clusters, every one
  attributed to absolute host-boot-base storage, behaviorally inert:
  the stored `wall_base_us` itself; guest RAM tick copies (tick−base equal:
  heap tick−base 395387100 both); a guest `ScePspDateTime`-shaped heap struct
  (seconds 40/34 = base%60 exactly, minutes 30/31 likewise, month 11,
  year 2026); a parked thread's saved tick register (base-consistent deltas
  within each run); two single GE-state bytes; trailing hash. Fresh-vs-fresh
  shows the same signature (earlier C-vs-D: 40 B, reports identical), so
  restore-vs-continuous now matches fresh-vs-fresh in kind.
- Legacy 24 B battle snapshot loads clean on the new binary and re-saves as
  40 B (+8 wall base, +8 phase across the two format steps).

Unit gates: `snapshot_tests` (wall continuity across simulated restart,
phase round trip, 24 B legacy fallback — each mutation-checked),
`snapshot_kernel_tests`, `snapshot_hle_tests`, `test_snapshot_surfaces`,
full python suite, `repository_check` — all green. Per project rules the
feature still counts as proven only after T15e manual device QA.

## 2026-11-01 — T15e manual device QA sign-off (VERIFIED)

**Status: proven on device.** `docs/qa/2026-11-01-t15e-snapshot.md` records
the full matrix (M2102K1AC, Android 14, lahaina/Adreno, build
`4a84003-dirty`, APK sha `26dc3ed…0aab9`). Scripted flow
(`scripts/android/qa-snapshot.sh`, EXIT=0): adb push of the host battle
snapshot (md5-verified on device), intent load (`snapshot_loaded`,
resume at `0x08805DEC`, frames flowing), 60 s restored play (battle stall,
then the game's own idle return to title — no faults, frames keep flowing),
Menu save (`snapshot_saved` + toast, session resumes), Menu load (post-load
frame md5-identical across menu load and both cold boots), 3× cold-boot
restores, corrupt file → `snapshot_truncated` named error with the session
stopped. No crashes (only script force-stops in exit-info).

**Incidental fix (device-verified):** the Menu panel overflowed short
landscape screens leaving Save/Load/Close unlaid-out and unreachable;
`SettingsOverlayView` now scrolls. This is the class of bug host tests
cannot catch — the T15e gate doing its job.

## 2026-11-02 — Battle BGM stall + system-menu list-save (both VERIFIED on device)

**Atrac starvation wedge (fixed).** The pre-battle event scene wedged with
`user_main` parked in `MsgPipeSend` on a full `SOUND MsgPipe(UMD)`, zero
`AddStreamData` calls ever, and every decode returning `BUFFER_EMPTY`.
Snapshot forensics (`.gsnap` parsed in Python) showed `bgm70.at3` open with
exactly the 175-frame initial window and atrac id 2 as a non-streaming
`SetDataAndGetID` prefix. PPSSPP `Atrac::DecodeData` decodes past the buffered
window but inside the file with synthesized silence and advances (return 0,
"so games don't infinite loop"); our `BUFFER_EMPTY` spun the UMD sound thread
out of its pipe-319 receive section. Fix mirrors PPSSPP (red-first
prefix-track test in `tests/hle/kernel_tests.cpp`); the earlier
`GetStreamDataInfo` moving-cursor fix is retained but was insufficient on its
own. Device proof: the enemy turn plays through on the clean build.

**Save-menu list-save gap (fixed).** The system menu Save called
`SceUtilitySavedataInitStart` mode 5 `LISTSAVE`; `perform_operation` had no
case 5 and swallowed it into the unimplemented default (fake success, no
file). Implemented `perform_list_save` with PPSSPP's `SavedataParam` focus
semantics (NAME index, FIRST/LASTLIST, LATEST/OLDEST by mtime,
FIRST/LASTDATA/EMPTY, all fall back to entry 0) plus a `perform_save`
override and request write-back, `kModeListSave = 5`, red-first host test.
Device proof: menu save plus menu load plus reset-and-load-from-title all on
the clean build; `ULUS10592GUN9THSTORY00/` (PARAM.SFO, STORY, icons) verified
on the memstick via `run-as`.

## 2026-11-03 — Backgrounding ends the guest session (device, FIXED)

**Symptom.** Press HOME while a session runs, then bring the activity back:
`lifecycle_paused`, the worker prints its `session_unbound` summary, then
`lifecycle_resumed 3063x1335` on return — with no `session_start` and no
further `guest_frame` lines. The screen keeps the last presented frame
indefinitely. Observed during the fast-forward QA cell
(`docs/qa/2026-11-03-m2102k1ac-fastforward.md`), at 17:28:03 local.

**Falsification.** Not a surface or rendering recovery problem: the swapchain
is recreated on resume (`lifecycle_resumed <w>x<h>`), the touch overlay and
the host fast-forward control keep working, and only guest work stops. Not a
fast-forward interaction: the stop is logged 227 ms after
`lifecycle_paused` and 3 s before the foreground event, and the host control
never touches the session thread.

**Root cause.** `LifecycleState::lifecycle_watch` called
`session->request_stop()` on `SDL_EVENT_WILL_ENTER_BACKGROUND`
(`android/app/src/main/cpp/main.cpp`) and **nothing ever resumed the guest**.
`run_session_worker` runs `session.run()` exactly once; the foreground branch
only resumed audio and rebuilt the surface. A stop without a resume is never
acceptable, and this one was invisible in the logs because the worker's
summary still printed.

**Fix (2026-11-03).** The session is stopped between dispatches on background
and resumed in place on foreground from the pc it stopped at, reusing the
`request_stop`/`request_resume`/`launch_session_thread` path the snapshot menu
already proved on device. `runtime/session/session_lifecycle.*` owns the
decision (stop a live session once; resume only a `HostStop`; never revive a
run that hit its dispatch or wall-clock bound; stay pending until the worker
has actually returned), and is unit-tested on the host. The relaunch runs
under the snapshot mutex, waits for the recreated surface, and
`GameSession::request_resume` calls `Kernel::resync_real_time()` so the resumed
run does not race the wall time it spent paused. The mixer is muted for the
whole background window: the paused AAudio stream drains nothing, so the few
hundred milliseconds the guest needs to stop would otherwise fill the ring and
count `overruns` on every round trip.

**Evidence.** `docs/qa/2026-11-03-m2102k1ac-background-resume.md`: battle scene
(2834 draws), same PID, one `session_start`, `guest_frame` lines 0.5 s after
`session_resumed reason=foreground`, unchanged HP/turn, `overruns=0` and
`xruns=0` across the round trip, three scripted cycles green.

**Retraction.** The "rendering" row of
`docs/qa/2026-11-03-m2102k1ac-resume.md` is **withdrawn**. Its evidence was a
`grep -c guest_frame` over the whole log buffer, which counts pre-pause lines;
the session had in fact ended. That report is marked superseded and the cell is
re-signed from the new record.

## 2026-11-03 — Startup SIGSEGV: the identity gate spent the SDL thread's stack (device, FIXED)

**Symptom.** The app crashed at launch with `SIGSEGV` in the `SDLThread`,
sometimes, and only on the phone: logcat stopped after `runtime_assets_current`,
the process died before any `gungnir` session line, and debuggerd reported
`Cause: stack pointer is close to top of stack`. It first appeared while adding
the T17a interpolation schedule, reproduced with the cadence gap counters, and
once even after reverting `main.cpp` — so it was never caused by that work, only
exposed by it: the margin was effectively zero and any change tipped it.

**Falsification.** Not a `main()` frame problem: the disassembly showed
`SDL_main` reserving about 5.8 KB, and adding locals there changed nothing. Not
an interpolation or pacing defect: the crash happened with the frame-rate work
reverted. The tombstone's backtrace named the consumer:
`SDL_main → gungnir::platform::select_game_data → verify_game_data →
istream::read → fread → malloc`, with `sp` one page above the stack bottom.

**Root cause.** `verify_game_data` kept `std::array<unsigned char, 1u << 20>` as
a local — declared after the size check, so it only materialised once a
candidate file matched the disc size. clang reports the frame as
**1,049,040 bytes**. The SDL thread is SDLActivity's Java thread, and the device
measures it at `limit_kb=1043 used_kb=9 headroom_kb=1033`
(`stack_space thread=main`): the buffer alone was the entire stack, leaving
about 10 KiB of margin, so the first call the stream made afterwards (a
`malloc` for the file buffer) crossed the guard page. The session worker's
`std::thread` is no better (`limit_kb=995`), so the same class of bug could take
the guest down later.

**Fix (2026-11-03).** The buffer is a heap `std::vector` sized by
`kVerifyChunk`. Because the failure is invisible until it runs on the device,
three gates landed with it: every runtime library in both build roots compiles
with `-Werror=frame-larger-than=16384` (`cmake/BoundedStack.cmake`), so the old
code fails the build with a 1,049,152-byte frame; the identity gate is exercised
on a thread given 512 KiB of stack (`game_data_tests`, failing with the signal
when it does not fit); and `stack_space thread=<name>` logs the limit, use and
headroom of the SDL thread and the session worker at startup. `scripts/android/qa-startup.sh`
repeats the cold boot ten times and fails on any crash, so one good boot never
counts as evidence again.

**Evidence.** `docs/qa/2026-11-03-m2102k1ac-interpolation.md`: ten consecutive
cold boots, `runs=10 failures=0`, each reporting
`stack_space thread=main limit_kb=1043 used_kb=9 headroom_kb=1033` and
`stack_space thread=session limit_kb=995 used_kb=0 headroom_kb=995`, then the
whole T17a.8 cell on the same artifact.

**Regression tests.** `game_data_tests` (identity gate fits a 512 KiB stack),
`stack_space_tests` (limit, use and headroom are reported and track frame
depth), `tests/test_startup_limits.py` (the frame bound is applied in both build
roots and the cold-boot gate exists and fails on crashes). Gotcha §7E.9.

**Retraction.** The T17a investigation's own suspicion that the frame-rate work
had introduced the crash is withdrawn: the tree crashed with that work reverted,
and the cause sits in `runtime/platform/android/game_data.cpp`, untouched by it.

## 2026-11-04 — Interpolation made the battle scene shake

**Symptom.** With frame interpolation on, every character sprite shimmied while
the battle cursor moved and the map panned. Reported against the build that had
just gained adaptive widescreen, so the widening was the first suspect.

**Falsification.** The flicker is absent with interpolation off and present with
interpolation on and widescreen off (`--es widescreen off`), on the same
snapshot, so it is not the coverage classification or the widening. A ten-second
display recording of the pan shows no per-frame geometry oscillation at the
presentation rate, which is what a scheduling or blend error would look like; the
artifact is per draw and only during motion. A one-shot `interpolation_pairs`
counter confirmed the mechanism on the device: while idle every counter is zero,
and while panning 36-66 draws per present were rejected by the cut test and 40-53
received a different partner than arrival order would have given them.

**Root cause (three defects, all in the interpolator).**
1. Partners were chosen by arrival index inside an identity bucket. The battle
   draws one character's sprite as several quads of one sheet, and the order
   inside that bucket is not stable: a cursor highlight, an animation frame or a
   culled sprite adds or drops a draw and shifts every later one. Each quad was
   then blended toward its neighbour's previous position, tearing the sprite
   apart. That is the reported shake.
2. The cut test was per draw at 0.2 of the frame, and a panning battle map
   reaches that on its fastest content. A rejected draw is shown un-lagged while
   its neighbours stay half an interval behind, and which side of the limit a
   draw sits on flapped between presents: a per-draw velocity shake at the
   threshold.
3. The obvious fix for (1), matching every bucket by nearest position, is wrong
   for repetitive scenery: the nearest previous tile is the *neighbour* as soon
   as the pan moves further than the tile spacing, so the whole ground drags
   backwards by one tile per present. Measured on the device: one bucket holds
   546 draws (the map's tiles of one texture), and matching it by position
   flickered the scenery while it fixed the sprites.

**Fix.** Buckets of up to `kMaxBucketScan` (64) draws pair by nearest previous
position, closest pairs first, so a surplus draw stays unmatched instead of
stealing a partner. Buckets past that keep arrival order and are counted
(`oversize`, `oversize_draws`) rather than silently mismatched: scenery of one
texture is a regular lattice, and arrival order is the only correct and
affordable choice there. `kCutScreenFraction` moves from 0.2 to 0.5 of the frame
(ten screens a second), above anything a pan reaches; partner choice is what
protects a cut from being smeared now, and the distance test is only the
absurd-partner backstop.

**Regression tests.** `tests/ge/interpolation_tests.cpp`: a bucket that gained a
draw, a bucket that lost one, a fast pan that must be interpolated rather than
cut, a panning tile bucket that must keep arrival order, and the oversize
fallback being counted. Three mutations: matching every bucket by position kills
the tile contract, matching every bucket by arrival order kills the gained-draw
contract, and restoring the 0.2 cut fraction kills the fast-pan contract.

**Evidence.** Device, battle snapshot, M2102K1AC, one log tick = one second of
passes: idle `candidates=2585 matched=2585 cut=0 reassigned=0 displaced=0
oversize=10 oversize_draws=2148 largest_bucket=546`; during an adb stick pan
`reassigned=24..58 displaced=24..57 cut=6..22` over slow and fast pans, against
`reassigned=41..53 cut=36..66` with the old 0.2 cut fraction while panning the
same way. What remains of `cut` after the fix is the absurd-partner backstop: a
small bucket whose real partner left the frame hands the draw a distant one, and
that pair is rejected instead of dragged. The pan figures are averages per pass, so
`reassigned` there is the number of draws per present that arrival order would
have blended toward the wrong object. 2148 of the 2585 candidates live in the
ten buckets past the scan limit, which is why the scenery fallback matters.

**Retraction (2026-11-04, later the same day).** Three shipped builds later, the
device said the shake was still there: position matching for every bucket fixed
the sprites and made the scenery flicker; keeping arrival order unless the count
changed restored the shake; raising the cut fraction changed the cursor's
animation instead. That falsifies the root cause above: cross-pairing is real and
worth the contracts that now pin it, but it was never the shake.

**Actual root cause.** A bounded dump of eight real battle frames
(`--es dump_draws <path>`) settled it: the whole draw stream is orderly. No
bucket's arrival order is broken while panning (largest arrival delta 42 px
against a 17 px median), the guest's own frame trajectory is smooth and
monotone, every blend is geometrically sound (no collapsed or inverted quads),
and the presentation timeline never regresses (`timeline presents=394
regressions=0 per_frame=2..3`). What the dump *did* show is that Gungnir submits
its **battle unit sprites in through mode**: eleven screen-space draws of
30x47 at indices 2214-2224, world-anchored, moving with the camera like the map
tiles. `interpolatable()` skipped the whole through class, so the units moved in
20 Hz steps while the map behind them was smoothed at 40-60 presents per second.
Every unit therefore visibly stepped against a smoothly panning background, and
that is the "shimmy and shake". It is widescreen-independent, invisible with
interpolation off, and immune to any matching change, which is exactly what the
three failed attempts looked like from the device.

**Fix.** Screen-space draws are candidates like any other; identity decides what
is the same draw. A draw whose identity changed has no partner and is shown as
produced (so HUD text and glyphs do not smear), and static HUD is identical in
both frames, so blending it is a no-op. Counters confirm the class now
participates: `candidates=2793 matched=2792` (was 2585/2584) with the same 208
screen-space draws per frame.

**References.** `BasedYuki/the-3rd-birthday-recomp` skips through draws too;
that is a property of its title's 2D layer, not of the GE. Recorded in
`docs/SOURCE_PROVENANCE.md`.

## 2026-11-04 — Two interpolation rules withdrawn (doors and meshes)

**Symptom.** After the sprite and collapse fixes, doors and level meshes started
misbehaving in ways the reporting player did not remember from before the work.

**Falsification.** Both suspect rules were mine, added during that work, and the
device dump measured them: (a) judging a draw against its own identity bucket's
median motion rejected 8-46 draws per frame pair that were merely fast members of
an otherwise static bucket — a door opening is exactly that; (b) treating a large
per-vertex deviation as a shape change translated **468-975 meshes per frame
pair** rigidly. Those meshes (keys 0x29a8cba2, 0xbe777b42, 0x9f137210,
0x71f24337 — walls, edges, structures) are only perspective-foreshortened: their
corners move by different amounts while the shape is unchanged, so vertex-wise
blending was correct and the rigid path removed it.

**Root cause.** Both rules were fitted to a real but narrow artifact (a scrolling
list wrapping its last item, and particles collapsing mid-spin) and made the
interpolator stricter than the design it replaced. A stricter rule can only be
justified if the original was visibly wrong in that case; here the original was
fine and the fix caused a different defect.

**Fix.** The bucket-relative rejection and the deviation test are removed. What
remains cannot be stricter than the original: the still-scene tolerance is back at
0.2 of the frame, and the frame-relative allowance (`kMotionSlack`) only *widens*
it while the scene is moving. The shape answer is now the collapse itself — a pair
whose vertex-wise blend would halve or invert any triangle is translated rigidly
(12-18 pairs per frame pair on the same dump, all genuine collapses). The
scrolling-list wrap is recorded as a **named, counted gap**: at rest it can still
be dragged by up to half its jump for one present; fixing it needs a rule that
distinguishes a wrap from a moving element, and neither the bucket median nor the
floor did so without breaking doors.

**Regression tests.** `a_foreshortening_mesh_is_blended_vertex_wise` (mutation:
re-introduce the deviation rule, the near edge is no longer bent halfway),
`a_slowly_moving_element_in_a_still_scene_is_blended` (mutation: floor back to 0.1,
a 60 px door becomes a cut), plus the retained collapse contracts.

## 2026-11-04 — Camera path trace: where the guest builds its projection

**Question (camera-hook track).** Before any hook can widen the guest's culling,
we need the function(s) that build the GE projection matrix, whether the cull
uses the same values or independent screen bounds, and whether one hook covers
both battle and menu scenes. No guessing: the deliverable is guest addresses and
instance counts from at least two bounded runs.

**Instrumentation (red-first).** `tests/test_camera_trace.py` (3 source contracts)
and `tests/test_camera_trace_report.py` (5 decode/grouping contracts) were written
before the code. `runtime/hle/hle_media.cpp` gained an opt-in, bounded
`GUNGNIR_TRACE_CAMERA` diagnostic that logs GE commands 0x3E (projection matrix
index) and 0x3F (projection matrix upload) with the enqueued list PC, the
command's own PC and the raw word; absent env var changes nothing, default cap
4096 lines (`GUNGNIR_TRACE_CAMERA_LIMIT` raises it). `tools/camera_trace_report.py`
groups the stream into 16-lane matrices and decodes the GE f24 argument (top 3
bytes of an IEEE float — pinned by contract).

**Evidence: two bounded host boots** (`boot_runner`, `--no-stop-at-flip`,
`--max-dispatches 5000000/2000000`, `--wall-seconds 20/12`; both exit 3 =
bounded out after the first flip, ~3948 / ~1470 GE frames). Both reproduce the
same sites:

| Run | projection commands | complete 16-lane matrices | distinct matrices |
|---|---|---|---|
| 1 | 117540 | 6914 | 4 |
| 2 | 51784 | 3046 | 4 |

The GE lists are two double-buffered frame lists (`0x09C51010`, `0x09C51850`)
plus one static boot list (`0x089DDC48`). Each frame list carries **two** 3D
projection blocks (e.g. `0x9C52238` / `0x9C5242C` and `0x9C72278` /
`0x9C7246C`) per frame — 1703 / 1702 instances in run 1 — and one 2D/HUD ortho
block (`0x9C52904` / `0x9C72944`) — 52 instances in both runs. So the same
projection builder runs for the whole scene; the 52 ortho instances are a
different mode of the same block, not a separate menu-only camera. A third
distinct matrix appears once in each run (a scene-transition frame), and the boot
list writes its own block once.

**Measured matrices (f24 decoded).** Main 3D projection: diagonal
`m00=0.082495`, `m11=0.145580`, `m22=-0.010000`, `m33=1.0`. The ratio
`m11/m00 = 1.7647 = 480/272` — exactly the PSP aspect — so the aspect is baked
into the matrix. HUD ortho: `m00=1/240`, `m11=-1/136`, `m22=-1`, translation
`(-1, 1)` — the standard 480x272 2D projection. Transition frame: `m00=1.215`,
`m11=2.144` (~14.7x scale), one instance per run.

**Where it is built.** `PSPRECOMP_WATCH_WRITE=0x09C52238` (68 bytes) on a third
bounded run logged all 17 command words of the block being written by guest PC
`0x0880528C` on thread `user_main`, every frame. In the generated corpus that PC
is the return epilogue of a call from `0x08805288` into the display-list builder
at **`0x088A3CAC`** (`generated_unit_0039.cpp`), which materializes six
`gpr[4] + 0x30000` pointers, reads a global at `0x08B19A10` and emits the GE
command words through helper calls at `0x088040D0` / `0x088040E4` /
`0x088040F8`. The first write of each frame sets the ortho matrix and the second
overwrites it with the 3D matrix at the *same* site, so one hook site serves both
modes.

**Cull evidence.** The captured frame JSON at GE frame 2110 reports scissor
`[0, 0, 480, 272]` on all 11 draws — fixed guest screen bounds, not derived
from the projection matrix. The host widescreen stretch (`StateMachine::
set_widescreen`) is presentation-only; the guest clips and culls to 480x272
regardless. **Conclusion: the cull is independent**, so patching only the
projection input is not enough — the hook must also account for the fixed
screen-space bounds (or patch the value that feeds both). This is a measured
statement about the observed frame, not a claim that every scene uses the same
bounds; a scene sweep is the blocked follow-up.

**Named gaps (not guessed).** The FOV/aspect *input* constant and the
fov->matrix conversion were not located: the matrix values are computed at
runtime (no literal float constants for 0.082495 / 0.145580 exist in the
generated corpus), and the exact instruction that produces `m00`/`m11` inside
`0x088A3CAC`'s callees has not been traced. `gh search code` hit a 403 rate
limit during the survey; the BasedYuki `versions/ULUS10567.toml` hook-site
reference (cam_projection / cam_setup) was already recorded. Next experiment:
watch the writes to the float source values that feed `0x088A3CAC`, and compare
PPSSPP as an oracle only.

**References.** Recorded in `docs/SOURCE_PROVENANCE.md`. PPSSPP remains a diff
oracle, never a link target.

## 2026-11-04 — Retraction: interpolation artifacts still present on device

**Reported (user QA, 30c1f958, deployed APK SHA 9999f508…).** "seems to be
working. all the same issues with the frame interpolation still exist though".
Confirmed present: **walls/meshes folding or collapsing** and **doors/meshes
snapping rigidly**.

**Retraction.** The earlier entries in this journal claimed the collapse was
fixed ("VERIFIED ON DEVICE (24-frame real pan): with the answer, 0 collapsing
triangles in every frame") and that the door/mesh regressions were withdrawn.
Those claims measured the **draw dump with the offline tool**, not what the
player sees on the device. They are **not validated by manual QA** and must not
be treated as closed. A zero count from `interpolation_report.py` is a property
of the dump and the rule, not proof that the displayed frame has no fold. The
dump may have been captured while idle, or the shape-change rule may not be the
mechanism of the visible artifact.

**Falsification plan (red-first, before any code change).** Capture a bounded
guest-frame dump and a screen recording **while the artifact is visible**, with
interpolation on and a real stick pan. Analyse the dump for: collapsing
triangles in the guest's own frames, pairs translated rigidly, and pairs whose
vertex-wise blend is applied to geometry that should not move that way. Compare
against interpolation-off frames from the same input. Only then write a
reproducing contract and fix.

**State.** The parent flicker defect task remains open; the user QA gate is
open; nothing committed. The per-draw matrix interpolation track is now
unblocked but not started —
the visible artifacts are the reason that direction exists, but a host-side
vertex blend may be the fundamental limit rather than a fixable rule.

## 2026-11-04 — Fresh pan dump: why the "0 collapses" claim was wrong

**Evidence.** Two bounded dumps with interpolation on at the battle snapshot on
`30c1f958`:

| Dump | Pan | Frames | Motion | Rigid pairs | Collapses |
|---|---|---|---|---|---|
| `pan.bin` (sha 371a8a32…) | missed the window | 24 | median 0.0, max 3.4 px | 0 | 0 |
| `pan2.bin` (sha 85447031…) | real stick pan | 24 | median 16–39 px, p90 ≤332, max 651 | 0–318 (6 frames >100) | 0 |

The first dump was **idle**: the swipe loop finished before the 6 s
`dump_after_ms` elapsed, so the earlier "VERIFIED ON DEVICE: 0 collapsing
triangles in every frame" measured an idle dump, not the artifact. That is the
concrete reason the retraction above stands.

The second dump, captured with `dump_after_ms=2000` and the pan overlapping the
window, shows real motion and a new, reproducible signal: the **collapse-only
rigid-translation rule fires on 287–318 draw pairs on every frame pair with
p90 > 200 px** (6 of 23 frames exceed 100 rigid pairs total). The runtime's own
1 Hz log agrees (`translated=72` averaged during the pan vs 0 at rest). A rule
meant for a handful of spinning quads is translating hundreds of draws rigidly
during fast pans — a plausible mechanism for the reported "doors/meshes
snapping rigidly".

**Hypothesis falsified, then root cause confirmed (same day, before any code
change).** A pure translation is translation-invariant: triangle area is
identical before, after and in the blend, so `would_collapse` can never fire on
one. Direct inspection of the 318 "collapsing" pairs in frame pair 2->3 of
`pan2.bin` showed they come from 9 keys, all but two of them OVERSIZE buckets
(508->314, 308->247, 237->185, 106->96, 77->58 draws) that therefore pair by
ARRIVAL ORDER. Both bucket size and member order change during a pan, so
arrival index N in the current frame is a different object than index N in the
previous frame's larger, reordered bucket. The exact offending pair (arrival
index 12, key `0xac4536df81a6dc17`) has before=-12.1, after=-2.0, but the
vertex-wise blend flips winding (middle=+132.9) because the two quads have
folded corners and different shapes. The collapse rule is CORRECT about that
pair; the PARTNER is wrong. Runtime counters agree: `reassigned=134
displaced=133` during the pan, `order_breaks` up to 43.
RED-FIRST REPRODUCTION LANDED (failing): `a_pan_of_repetitive_wall_quads_
keeps_each_quad_with_its_partner` in tests/ge/interpolation_tests.cpp uses the
REAL device vertices of that pair and asserts no rigid translation; it fails
with "two different objects must not be rigidly translated as one". Three
synthetic approximations were tried first and all passed - the real data was
needed to reproduce it.
Root cause: partner identity within a changing oversize bucket. Neither a
rejection rule nor a translation rule can supply the missing identity; this is
why per-draw transform identity is the relevant direction.

**Still unexplained.** The reported **walls/meshes folding** appears in neither
endpoint frame of `pan2.bin` (`--vertex-only` also reports 0 collapses). Either
the fold exists only in the *interpolated present* between two frames, or it is
a winding/area artifact the offline tool does not count. Next experiment: dump
the interpolated presents (not just guest frames) or record the screen during a
pan and compare. No code change until both artifacts have a reproduction.

## 2026-11-04 — Direction chosen: per-draw matrix interpolation

**Decision (user).** Of the two unblocked tracks, per-draw matrix
interpolation is the one related to the persisting frame-interpolation
artifacts; the camera/widescreen hook addresses a different symptom (guest
culling to a native 480-wide frustum) and is not the interpolation track.
Work starts on per-draw matrix interpolation.

**Rationale.** The two reported artifacts are one limitation seen from two
sides. The host only receives already-projected vertices, so an in-between
built by blending them cannot know the underlying rigid transform:
- it *invents* intermediate shapes (linear blend of rigid transforms is not
  rigid — the candy-wrapper / folding artifact), or
- when the collapse rule detects that, it translates the whole draw rigidly
  instead (the "doors/meshes snapping rigidly" artifact). Fresh pan dump
  `pan2.bin` measured the collapse-only rigid rule firing on 287–318 draws on
  every fast-pan frame pair, so the workaround for the first artifact is
  manufacturing the second.

The mature answer, recorded in the extern survey, is RT64/Zelda64Recomp's:
interpolate **per-draw transform matrices**, decomposed into translation,
rotation (quaternion slerp) and scale, with explicit skip modes; use vertex
interpolation only for screen-aligned/UI quads. That removes the algebra that
produces both artifacts rather than patching each symptom.

**Prerequisites cleared.** Camera trace (the camera trace) and profile surface
(the profile surface) are complete; the profile is device-verified. Still missing, and
now the first real work: the per-draw transform input itself — the camera trace
found the projection builder at `0x088A3CAC` and the GE display-list sites but
explicitly left the FOV/aspect input and per-object transforms as a named gap.

**First deliverables (red-first, no speculative rules).** (1) Reproduce the
large-pure-translation collapse-test misfire as a host contract. (2) Dump the
**interpolated presents**, not just guest endpoint frames, to locate the fold
that appears in neither endpoint of `pan2.bin`. (3) Extend the trace to find
per-draw transform writes into `0x088A3CAC`. Only then add a per-draw transform
identity, slerp, keep the existing skip/cut rules, and validate on the battle
snapshot with the dump tool plus manual device QA. the parent flicker defect task stays open as the
parent defect record.

## 2026-11-04 — The folding artifact located in the interpolated presents

**Experiment.** Added a second bounded, opt-in dump (`--es dump_presents <path>`)
sharing the guest-frame dump format, written from `to_present` (the frame the
presenter actually shows, including interpolated in-betweens). Captured during
a real stick pan with interpolation on: `present_pan.bin` (sha
aba9b6f1…) and `guest_pan.bin` (sha 3c43c1ee…), 24 frames each.

**Result.** The guest endpoint frames show 0 collapsing triangles; the
**presented frames show 11–641 per frame pair**, with peaks of 599–641 on
frame pairs 7->8 and 11->12. `--vertex-only` reports `rigid_pairs=0` on those
presents, meaning the runtime's collapse rule did NOT fire on them, so the
folded geometry went straight to the screen. This is the reported
walls/meshes folding, reproduced with real device data.

**Mechanism.** The collapsing keys are the SAME oversize, changing wall-quad
buckets identified for the rigid snapping (`0xac4536df81a6dc17`,
`0x97280303e06e2915`, …). Direct inspection of a collapsed presented pair:
`before=-15.2, after=-10.7, blend=+107.2` — a winding flip. Blending the
folded corners of two DIFFERENT objects (paired by arrival order in a bucket
whose size and member order changed) inverts a triangle.

**Both artifacts are one root cause.** Wrong partner in a changing oversize
bucket: when the collapse rule catches the bad blend it translates the draw
rigidly (doors/meshes snapping); when it does not, the inverted triangle is
presented (walls/meshes folding). Neither a rejection rule nor a translation
rule can supply the missing per-draw identity. This is the measured
justification for the per-draw transform/interpolation direction.

**State.** The presented-frame dump is bounded, opt-in and off by default; it
never changes what is presented. Both dumps are app-private and gitignored.
No interpolation code has been changed in this investigation.

## 2026-11-04 — Transform trace: Gungnir has no per-draw matrices to interpolate

**Experiment.** Extended the bounded `GUNGNIR_TRACE_CAMERA` diagnostic to log
world (0x3A/0x3B), view (0x3C/0x3D) and bone (0x2A/0x2B) matrix commands as
well as projection, under a distinct `[camera_transform]` tag. Ran a bounded
host boot (1.5M dispatches, 12 s, `--no-stop-at-flip`) and wrote
`tools/camera_transform_report.py` (5 contracts) to group the stream into
12-lane blocks, decode f24 and count distinct matrices.

**Measured result (run6.log, 64,434 transform commands, 4,959 blocks):**

| Kind | Blocks | Distinct matrices | Content |
|---|---|---|---|
| world | 2479 | 3 (+1 partial) | identity `I`, `32768*I`, and `diag(32768,32768,1)` — all constant |
| view | 2479 | 3 (+1 partial) | one fixed isometric rotation (`0.7071, -0.3535, 0.6124, 0.866, 0.5, ...`, translation `-1.1547`), identity, and one boot frame with `-10` |
| bone | 1 | 1 | boot list only; no skinning matrices in the game scene |

Across 2,478 full 12-lane view blocks, **2,372 are the exact same constant
rotation matrix**; world matrices are likewise constant. Pan and object motion
are therefore **not** carried in GE transform matrices at all — the guest
computes already-projected vertices on the CPU each frame (consistent with the
earlier finding that its battle unit sprites arrive as through-mode
screen-space quads).

**Conclusion (changes the direction).** RT64/Zelda64Recomp-style interpolation
of decomposed per-draw transform matrices **cannot be applied directly to this
title**: there is no per-draw transform to interpolate. What the artifacts
actually need is a **stable per-draw identity** so the interpolator pairs the
right object across frames, not a transform decomposition. The measured root
cause (wrong partner in a changing oversize bucket) stands; the fix is an
identity/tagging mechanism, and the transform trace shows that mechanism must
come from the guest's CPU-side object data or display-list construction, not
from GE matrix registers.

**Named gap / next experiment.** Where the CPU-side object identity lives is
not yet known. Candidates: the display-list builder at `0x088A3CAC` and its
callees (`0x088040D0`/`E4`/`F8`), the global it reads at `0x08B19A10`, or a
per-object vertex source buffer that the write-watch can name. Watch writes to
those and to the vertex-list source, then look for a stable index/ID that
survives a cull-driven bucket reorder. PPSSPP remains a diff oracle only.

**State.** No interpolation code changed. The presented-frame dump, the two
red-first reproductions, and this transform trace are all evidence; the failing
contract stays red until an identity mechanism lands.

## 2026-11-04 — Per-draw identity hunt: bounded negative result

**Experiment.** Added an opt-in, bounded `GUNGNIR_TRACE_VERTEX_SOURCE`
diagnostic logging GE vertex/index address commands (0x01/0x02) with the
resolved address; `tests/test_vertex_source_trace.py` (3 contracts). Ran a
bounded host boot (800k dispatches): 22,307 vertex-source commands across
212 distinct (list, command_pc) sites, of which 202 map to exactly one address
(the other 10 are the 2D/HUD scene). Also watched CPU writes to a shared vertex
buffer with `PSPRECOMP_WATCH_WRITE=0x097F7F50`: written by guest PCs
`0x08900000` and `0x088B636C` with packed screen-space vertex data
(color `0xff005050`, packed x/y like `0x820168`).

**Negative result.** Only **39 distinct vertex addresses** exist in the whole
run, and just **16 per frame list**, reused thousands of times (11,134
vertex-address commands in one list, 16 distinct addresses). These are shared
scratch buffers, not per-object IDs. Likewise there are only 94–97 distinct
vertex-address command PCs per frame list, while the offending wall-quad bucket
alone holds 508 draws. Neither the vertex source address nor the command PC
distinguishes the individual draws that mispair.

**Conclusion.** The guest builds a FLAT, CPU-projected stream of quads into
shared scratch buffers; culling reorders and resizes that stream each frame.
No host-visible field (GE transform matrices - constant; vertex addresses -
shared; command PCs - shared; draw-identity hash - content-based) carries a
stable per-object ID. The wrong-partner artifact therefore cannot be fixed by
tagging from existing host state.

**Named gap / next experiment (requires deeper reverse engineering, not
guessing).** A stable identity must come from a GUEST-SIDE hook that tags
objects BEFORE projection - i.e. the object-loop in the CPU renderer around
`0x08900000` / `0x088B636C`, or the display-list builder `0x088A3CAC`. Trace
that loop to find a per-object index or pointer that survives culling and
expose it through the per-title profile surface. Until then, a host-side
heuristic is the only alternative, and any such rule must be justified by a
red-first reproduction of exactly the wrong-partner case (the failing contract
already exists). Do not claim a fix without that.

**State.** No interpolation code changed. All trace diagnostics are opt-in and
bounded; logs and dumps are app-private/gitignored. PPSSPP diff oracle only.

## 2026-11-04 — CPU object-loop trace: no per-object ID (bounded result)

**Experiment.** Traced the CPU vertex-emission routines named by the write
watch. `0x08900000` (generated unit 0063) walks a structure with a +48/+64
stride and emits 16-byte vertex records; `0x088B636C`/`0x088B63AC` (unit 0044)
emit the same records for another batch. Extended the vertex-source diagnostic
to log PRIM commands (0x04) with their command PC and vertex count
(`tests/test_vertex_source_trace.py` extended). Ran two bounded host boots.

**Measured.**
- `0x08900000` is a large vertex-emission routine, not a clean per-object
  loop: it builds 16-byte vertex records into shared scratch buffers. The
  loop back-edge is driven through `invoke_chained_direct` calls
  (`0x0881BC28`/`0x0881BC5C`), so the object index lives in a caller, not here.
- PRIM commands per frame list: **94–97 distinct (list, command_pc) slots**,
  each repeated ~380 times (once per frame), with `count` values of 2, 32 and
  102. The 508-draw wall bucket is emitted from a few such slots; the same slot
  emits different objects in different enqueues because culling reorders the
  stream. So (list, command_pc) is a stable SLOT, not a per-draw identity.
- Watching writes into the shared vertex buffer showed 248 (guest PC, address)
  sites; only 20 vary, and all 20 vary in **screen coordinates** (x stepping
  130, 124, 117, 111, 105 … during a pan). There is no spare per-object index
  or pointer word in the vertex record.

**Conclusion.** The trace did NOT find a stable per-object identity. It did
establish where the identity would have to come from: the caller of
`0x08900000` (via `0x0881BC28`/`0x0881BC5C`) that holds the object index, or a
guest-side hook into the display-list builder `0x088A3CAC` before projection.
That is deeper reverse engineering of the object array, not a host-visible
field. No interpolation code changed.

**Named gap, final form.** A per-title hook must expose a per-object tag from
the CPU object loop before projection. Until that exists, the wrong-partner
artifact has no exact host-side fix; a heuristic would need its own red-first
reproduction (the failing contract exists) and must not be claimed as a fix
without it. PPSSPP remains a diff oracle only. All diagnostics are opt-in and
bounded; logs and dumps are app-private/gitignored.

## 2026-11-04 — GitHub survey: the wrong-partner artifact is known and has a solution shape

**Trigger.** The user pointed out `gh search -- "query"` syntax. Re-ran
reference discovery with repo-scoped `--repo` searches (30 search calls
available; broad queries returned unrelated noise).

**Issues found.** `Zelda64Recomp/Zelda64Recomp#61` (scene cuts / instant pose
changes warp under interpolation; closed as an original-game artifact
exaggerated by high frame rate) and `#562` (only texture scrolling is
interpolated in 2D). `rt64/rt64#93` is the direct one: "Add
gEXPreviousMatrix command to override the previous frame's transform for a
given draw", explicitly for **"when a game teleports meshes in order to
implement infinite scrolling"** — a bucket whose membership changes between
frames, our exact case. The maintainers' answer is a per-draw
previous-transform override, i.e. a stable per-draw tag supplied by the game
side, not a host heuristic.

**Implementations found (behavioural reference, no code copied).**
- `bryantmh/rush2recompiled:src/interpolation.cpp` opens with the exact
  problem: "Without tags RT64 pairs matrices between frames by guessing
  (`G_EX_ID_AUTO`), which **mismatches identical objects such as cars lined up
  at the start of a race**." Their solution: per-title hooks (`us.toml` ->
  `rush2_interp_*`) tag every draw with a stable ID derived from the GAME'S
  OWN object-pool slot index (scene graph node index in a 0x38-byte-record
  pool, world-space polygon slot "stable between frames", per-view camera
  index). IDs carry a generation that changes on teleport/camera cut so the
  interpolator snaps instead of sweeping.
- `Zelda64Recomp/Zelda64Recomp:patches/transform_ids.h` +
  `patches/*_transform_tagging.c` assign explicit ID ranges per object class
  (particle index, actor slot/limb, effects) at draw time via
  `gEXMatrixGroupDecomposedSkipAll(...)`. `patches/patches.h` also defines
  `gEXMatrixGroupNoInterpolation`.
- `rt64/rt64:src/hle/rt64_game_frame.cpp` pairs groups between frames with
  `curGroup.matrixId == prevGroup.matrixId`; `G_EX_ID_IGNORE` (0x0) is an
  explicit per-draw "do not interpolate" used by several recomp projects.

**Conclusion.** Our bounded negative result (no host-visible per-object ID) is
consistent with the literature: the tag must be **injected by a guest-side
per-title hook from the game's object pool**, exactly as rush2recompiled and
Zelda64Recomp do. The failing contract and the presented-frame dump give us a
red-first reproduction to validate such a hook when it lands. This is a
solution shape, not an implemented fix: finding Gungnir's object pool and
injecting IDs is still per-title reverse engineering. The alternative of an
explicit per-draw SKIP for untaggable buckets is also supported by the
literature (`G_EX_ID_IGNORE`), but would have to be counted and named, never
silently disabled.

**State.** No interpolation code changed. Full details and URLs in
`docs/SOURCE_PROVENANCE.md`. PPSSPP remains a diff oracle only.

## 2026-11-04 — Object-pool trace: not found; simple heuristic falsified

**Experiment.** Traced the callers of the vertex emitter and the display-list
builder in the generated corpus:
- `0x0881BC28` / `0x0881BC5C` (unit 0005) are generic container iterator
  helpers (`gpr[6] << 4` = 16-byte stride, base/end pointers at struct +24/+28/+32/+36), not the object loop.
- `0x08900000` is tail-called from `0x088FFFFC` (unit 0062); the surrounding
  loops (`0x088FF544`, `0x088FE7C0`, …) are calls to a quad-emitter subroutine,
  not an object-array iteration with a visible index.
- The display-list builder `0x088A3CAC` is called ONCE PER FRAME from
  `0x08805288` with a scene-context pointer `gpr[4] = 0x08B2C6E0`; its own loop
  (`0x088A390C` ← `0x088A3DEC`) is a retry/state loop over a global flag at
  `0x08B19A10`, not a per-object loop.
- Watching writes to the display-list PRIM slot `0x09C52230` attributes every
  store to `pc=0x0880528C` (the dispatch boundary after the builder returns),
  so the write-watch cannot distinguish individual draws inside the builder.

**Result.** Gungnir's per-object pool / slot index was NOT located. The guest
reorders the draw stream inside a per-frame builder, and no host-visible loop
counter or object pointer is exposed at the traced call sites.

**Heuristic falsified too.** An obvious host-side rule — "skip interpolation for
a bucket whose size changed between frames" — was checked against both dumps:
the wall bucket changes size on some frame pairs (508→314) but is CONSTANT on
others (508→508, 445→445) that still show `order_breaks=10-25` and
`rigid_pairs=6-28`. So membership reorders without a size change and the guard
would not fix all cases. Another rejection/translation rule is therefore not
justified.

**Conclusion.** The GitHub survey's solution shape stands as the only
well-founded path: a guest-side per-title hook that tags draws from the game's
own object pool (like `bryantmh/rush2recompiled:src/interpolation.cpp` and
`Zelda64Recomp:patches/*_transform_tagging.c`). Finding that pool is a larger
reverse-engineering effort into the per-frame builder's object loop, explicitly
recorded as a named gap — not guessed. The explicit per-draw SKIP
(`G_EX_ID_IGNORE`) remains a documented alternative but must be counted and
named; a size-change guard has now been falsified as a substitute.

**State.** No interpolation code changed. All diagnostics opt-in/bounded.
Nothing committed.

## 2026-11-04 — Battery net-discharges while plugged in (device, ROOT-CAUSED, no fix yet)

**Symptom.** User device QA: "significant battery drain on the device even
though it is plugged in." User clarified the screen is at ~60% brightness, not
full brightness.

**Falsification.** Several obvious explanations were checked and rejected with
measurements on `30c1f958` (M2102K1AC, 3200x1440, 120 Hz display, v0.1.0):
- *Broken guest pacing / free-running game* — FALSE. Guest is correctly paced
  at 20 Hz (`flips` +100 per 5 s tick; cadence `delay_us` +5,000,000 per 5 s =
  real-time-locked). `Kernel::pace_to_real_time()` is working.
- *Runaway log/dump I/O* — FALSE. No dump files active; ~9 logcat lines/5 s.
- *Full-brightness screen* — FALSE (user-corrected; `screen_brightness=32/255`,
  manual `mode=0`). The screen is a minor contributor.
- *Busy-spin in the present wait* — mostly FALSE. `FrameQueue::wait_for_next`
  is a proper condition-variable wait, but `sleep_for_next_frame` caps the wait
  at 8 ms, so the main loop wakes ≥125×/s even when nothing is due.

**Root cause (proven).** The app's sustained compute/render load exceeds the
weak USB charger, so the battery NET-DISCHARGES while Android still reports
`status=2` (charging):
- `AC powered: false`, `USB powered: true`, `Max charging current`
  580000–675000 µA (~650 mA weak USB).
- Battery **level 24 → 23 over ~6 s** with `status=2` = confirmed net
  discharge. SoC thermal zones 51/50/46 °C.
- Sustained **85–86 % of one CPU core**, rock-steady over 3/7/8/10 s windows
  (`top`: 85.1 %, 74:28 accumulated CPU time); dominant `SDLThread`,
  `PR 10 NI -10`. No idle.
- `present_guest_frame` costs ~10.9 ms wall each — full vertex copy +
  `to_vulkan_vertex` + widescreen per-vertex transform + Vulkan submit of
  hundreds of draws.

**Controlled A/B (live JNI toggle, same idle static scene, no restart).**

| | interpolation ON (user setting) | interpolation OFF |
|---|---|---|
| CPU (one core) | 86 % | 63 % |
| presents | ~39 Hz (2× guest) | ~20 Hz (1× guest) |

Interpolation ON adds +23 percentage points of a core (~37 % more CPU) by
doubling the present rate on a 120 Hz display — and it does so on a **static**
scene (`interpolation_pairs motion=0.0`, ~2793 candidate pairs/pass,
`largest_bucket=546`): pure recompute of identical frames. But OFF still burns
63 % of a core, so interpolation is an **amplifier, not the sole cause**.
Setting was restored to ON after the test.

**Contributors, largest first.** (1) Continuous ~85 % single-core CPU from
guest execution + per-present full vertex work. (2) Interpolation targeting the
120 Hz display refresh (`PresentSchedule` sets `next_tick_ = now + 1/120 s`),
so a 20 Hz game is presented at up to 120 / achieved 39 Hz. (3)
`measure_widescreen_coverage` runs **every present, always-on, with no
diagnostic gate** — QA instrumentation sitting in the production render loop
(added this session). (4) The 8 ms main-loop wake cap. (5)
`SCREEN_BRIGHT_WAKE_LOCK` + `PARTIAL_WAKE_LOCK 'AudioMix'` (WorkSource 10557)
keep display/SoC awake continuously. (6) Moderate screen brightness = minor.

**Open gaps (explicit, counted).** Whether this is a NEW regression vs a prior
build is unconfirmed (the coverage instrumentation and the 120 Hz-aware
interpolation schedule are recent). Absolute per-app mA draw is not exposed by
this device. No fix has been applied: candidate directions (skip interpolation
on static scenes, gate coverage to opt-in, cap the interpolation present rate,
lengthen the idle wait, honor 60 Hz) each require a red-first test, a mutation
check, and the manual device QA gate before any commit.

**Evidence.** `build/qa/battery/measurements.txt` (gitignored, sha256
`f3b61102f40677a8833f1707c8d7588b282472963ea2e22b66432620dcbbf714`). No game
content committed. Nothing committed; no code changed for this finding.

### Fix implemented (same day): static-scene skip + opt-in coverage

The user chose two of the candidate directions. Both were implemented
red-first with mutation checks; device QA record in
`docs/qa/2026-11-04-m2102k1ac-battery-fix.md`, raw numbers in
`build/qa/battery/fix-verification.txt` (gitignored).

**What changed.** (1) `scene_motion(current, previous)` measures mean
per-vertex screen motion between consecutive guest frames by arrival order
(draws whose vertex count changed are skipped, so a culling stream does not
look like a pan); `scene_is_static` compares it to `kStaticScenePixels = 1.0`
guest px. (2) `PresentSchedule::on_guest_frame` takes a `static_scene` flag:
when set it presents the real frame once and opens **no** interpolation window,
so `due()` yields nothing and no `interpolate_frame` pass runs for a still
picture. (3) `measure_widescreen_coverage` is now gated behind
`g_coverage_enabled`, an opt-in `--el coverage 1` intent extra, default off.
`static_frames` is counted and logged so the optimisation is visible, not
silent.

**Measured on device** (same battle snapshot, interpolation ON, 120 Hz):

| | old | new |
|---|---|---|
| sustained CPU (one core) | 85-86 % | **58-61 %** |
| presents per guest frame | ~2 (39 Hz) | **1 (20 Hz)** |
| interpolated presents, idle | ~196/5 s | **0** |
| `static_frames` (idle) | n/a | 292/294 (~99 %) |
| `present_time avg_us` | ~10920 | ~7645-8041 |

A pan still interpolates (`interpolated=35`, `motion=20.3`, `per_frame=0..3`),
so the skip does not disable the feature on moving scenes. Coverage is off by
default (no log lines, `present_time` drops) and on with `--el coverage 1`;
note `--es coverage 1` (string) does not work because `intent_long` reads a
long extra.

**Falsified by the fix.** "Interpolation is the whole drain" — turning it off
had given 63 %, but the static skip alone reaches 61 % with the feature still
ON, so the waste was specifically *interpolating a static scene*, not
interpolation in general.

**Named gaps.** A 12 s window held battery level 17 -> 17 at 58 % CPU, but
that is too short and the level is a coarse integer percent: **net charge
recovery is not claimed**; a longer charge-rate test on a high-current charger
is needed. A lone small moving element in a still scene is now held at the
guest rate (intentional trade at the 1 px threshold); watch it in the scene
sweep. Whether the drain was a regression is still unconfirmed.

**State.** Host `ctest` 2/2, `repository_check` PASS, APK builds. Nothing
committed; manual QA (three cold boots, device matrix, sign-off) pending the
user gate.

### Phase 2 — PPSSPP-derived pacing improvements (same day)

A reference survey of PPSSPP's frame pacing (`docs/SOURCE_PROVENANCE.md`,
2026-11-04) suggested three further changes; all were implemented red-first
with mutation checks. Child tasks `eacou9zl` (cap), `x0d44mv7` (wait),
`7rb8oi1d` (hold) under the battery task. Raw device numbers:
`build/qa/battery/phase2-verification.txt` (gitignored).

**(1) Cap the interpolation present rate.** PPSSPP's `FramerateTarget()`
clamps to 60 Hz on 90/120/144 Hz displays; ours had chased the host refresh.
Added pure `cap_refresh_hz()` + `kMaxPresentHz = 60.0` in `present_schedule`,
wired into `main.cpp` (`display_refresh_hz=120.0 capped_hz=60.0` on device).
A pan still interpolates but `per_frame` drops from `0..3` to `0..2`. Red test
+ mutation check ("a 120 Hz display is capped to 60 Hz") green.

**(2) Precise deadline wait.** PPSSPP's `WaitUntil()` sleeps to the real
deadline; ours clamped every idle wait to 8 ms. Added pure
`idle_wait_slice(now, deadline, ceiling)` in `frame_queue` and rewrote
`sleep_for_next_frame` to wait to `schedule.next_tick()` or
`frame_queue.pending_deadline()`, bounded by a named 16 ms responsiveness
ceiling. **Tradeoff, documented:** input is pumped only by the main loop's
`SDL_PollEvent` (no separate pump thread), so a longer sleep adds touch
latency; the ceiling is one display frame. Device check: tap → toggle logged
within 101 ms (adb polling overhead included), no perceptible regression. Red
test + mutation check green.

**(3) Hold the last image for identical frames.** PPSSPP's `__DisplayFlip()`
skips the flip entirely when `FramebufferDirty()` is false. A temporary
instrumented build proved that on this device (`VK_PRESENT_MODE_FIFO_KHR`)
SurfaceFlinger **retains the last presented buffer for ~53 s with zero
presents** (screenshots in `build/qa/battery/`, guest ran to frame 1140, no
surface errors). So the mechanism is feasible. Added exact
`frames_identical()` + `HeldImageState` with a `kHoldRePresentFrames = 10`
safety re-present (PPSSPP's 10-vblank fallback idea) and a counted
`presents_skipped` log; reset on continuity breaks and surface recreation.

**Falsification (important).** A dump of 8 consecutive guest frames on the
*same* static battle scene showed they are **not byte-identical**: 0–4 draws
differ per pair and mean xy motion is 0.003–0.027 px (sub-pixel jitter from a
blinking cursor/particle or culling). Therefore the exact hold triggers on
**0 of 7** pairs here (`presents_skipped=0` on device) — it only helps
genuinely frozen screens (menus, pause), not this scene. A threshold-based hold
using the 1 px `scene_is_static` signal *would* capture the remaining ~20
presents/s of CPU, but risks freezing subtly animated HUD/cursor elements for
up to the safety interval — a visual regression automated tests cannot catch.
**Not implemented**; recorded as a named gap, not papered over.

**Combined device result:** CPU **58 %** of one core (original 86 %, phase-1
fix 61 %), `present_time avg_us` ~7027 (original ~10920), zero idle in-betweens,
`static_frames` ~99 %, pan still interpolates at ~59 % CPU.

**State.** `ctest` 2/2 (158 s), `repository_check` PASS (506 files), all ge and
Python suites green, APK BUILD SUCCESSFUL. Nothing committed; manual QA (three
cold boots, device matrix, sign-off) pending the user gate. Net charge recovery
still not claimed (weak 191 mA charger, coarse battery level).

## 2026-11-05 — T13h: full Python suite blocked on host cmake/ZLIB (environmental, pre-existing)

`tools/run_python_suite.sh` (log `build/test-logs/python-suite-20261105T212251Z.log`):
205 tests, 23 setUpModule errors, all `cmake configure failed` at
`FindZLIB.cmake` (`ZLIB_INCLUDE_DIR`) — the host build tree cannot configure,
so every test needing built host tools (test_ge, test_hle, test_intake, …)
errors before running. Unrelated to the T13h changeset (ledger criteria,
PROGRESS/RELEASE docs, `tools/milestones.py`, two new test files); T13h's own
10 tests pass standalone and `tools/repository_check.py` passes (511 files).
Not pursued inside T13h: host toolchain setup belongs to the build
environment, not the release ledger. T13 closure still needs a green suite
plus T14c sign-off.

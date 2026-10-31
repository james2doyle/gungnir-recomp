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

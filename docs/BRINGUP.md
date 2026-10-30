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

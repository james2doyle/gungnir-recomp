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

An unbound import stops with

```
Missing HLE import <library>::<name-or-unknown> (NID_0x…)
```

It never returns a silent zero. `tests/test_hle.py` covers the named trap, the
cross-library alias path, and a bound handler invocation.

## Boot set

| Area | State | Notes |
|---|---|---|
| Registrar + NID registry | VERIFIED (T6a) | `runtime/hle/hle_common.*` |
| Kernel (cooperative threads, semas, eventflags, mutexes, VTimers, memory partitions) | PLANNED (T6b) | adapting the MIT Yakumo kernel at the pinned commit |
| ThreadMan / SysMem / IoFileMgr | PLANNED (T6b) | unit tests are TC-6.1 |
| Display / GE / Ctrl / Power / RTC / LoadExec / Stdio | PLANNED (T6c) | first `sceDisplaySetFrameBuf` is TC-6.2 |
| ModuleMgr PRX overlay tiers | PLANNED (T6d) | translate-on-load, unregister-on-unload (TC-6.3) |

## Provenance

The kernel/HLE design is adopted from TeamGDB/Yakumo (MIT, pinned
`b61852a`), with profile-specific pieces removed and recorded in
`third_party/yakumo/PROVENANCE.md`. NID data provenance is in
`third_party/pspsdk-nids/PROVENANCE.md`. No emulator code is linked.

## Commands

```sh
python3 -m unittest tests.test_nids tests.test_hle -v
```

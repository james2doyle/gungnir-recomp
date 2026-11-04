# Host save states (T15)

Host-only resume points for `GameSession`: snapshot to a file, restore it
later, continue the guest from the same point instead of replaying the
prologue. Snapshots embed guest RAM, i.e. game data: they live under
gitignored `build/qa/` on the host and in app-private storage on device,
and are **never committed, packaged, or uploaded**.

## Container

`GSNP` v1, little-endian, deterministic field order so two snapshots taken
at the same dispatch count are byte-identical:

- header: magic `GSNP` (4), version u32 (`1`), corpus identity (32),
  session config identity (32), section count u32
- sections in fixed order, each `tag u32 + length u32 + payload`
- trailer: FNV-1a/64 over all preceding bytes

Writes are atomic (temp file + rename). Loads are fail-closed: header,
hashes, order, lengths and the trailing hash are verified before anything
is committed, and a rejection names its reason (`snapshot_truncated`,
`snapshot_bad_magic/version/corpus/config/hash/tag/length`,
`snapshot_incomplete`, `snapshot_duplicate_uid`, `snapshot_duplicate_section`,
`snapshot_host_wait`, `snapshot_busy`, `snapshot_dangling_wait`,
`snapshot_open_failed`, `snapshot_write_failed`).

Identities: the corpus hash binds the guest ELF plus the snapshot
toolchain tag (`gungnir-snapshot-toolchain/v1`, bumped when codegen
changes what snapshots mean); the config hash binds `load_base` and
`boot_path`. A mismatch fails the load instead of running a foreign state.

## Section tags

| Tag(s) | Contents |
|---|---|
| 1–2 | Guest RAM (32 MiB), EDRAM (2 MiB), byte-for-byte |
| 3 | Runner CPU context (GPR/HI/LO/PC/FPR/VFPU, 848 B) |
| 4 | Virtual clock (`now_us`, `next_vblank_us`, `vblank_count`, absolute) + host wall-clock base (`wall_base_us`, keeps `sceRtc` ticks continuous across a process restart) + Runtime starvation-boundary phase (`dispatch_phase`, keeps future preemption boundaries and thread interleaving identical; 40 B total; 24/32 B legacy files without the later fields still load and default them to current/zero) |
| 5–6 | Kernel IDs (`next_uid`, current thread, sequences) + thread table (saved contexts, statuses, absolute wait deadlines) |
| 7–16 | Kernel objects: semaphores, event flags, mutexes, VPL/FPL pools, message pipes, callbacks, VTimers (absolute schedules), memory blocks + free ranges, sub-interrupts |
| 17 | Open file descriptors + positions (reopened against the live disc/memory stick; async windows preserved) |
| 18 | Audio channels (reservations, sample state, parked waiter) |
| 19 | SAS cores (voice cursors, envelopes, reverb settings) |
| 20 | ATRAC slots (source bytes + play positions; decoder overlap is re-primed, so PCM at the resume point is best-effort while IDs/positions/loops are exact) |
| 21 | GE/display state: display registers, control latch, GE callbacks/lists (runners rebuilt from guest RAM, including call stacks), state-machine registers/matrices/CLUT/addresses, frame counters |
| 22 | Module registry (metadata only; corpus installers are re-established by the boot path) |
| 23 | Savedata + message-dialog status machines (parameter blocks live in saved guest RAM; memory-stick contents are session config and are never snapshotted) |

## What is NOT saved (rebuilt, never faked)

- Mixer/AAudio timeline and the SAS staging buffers: dropped. Load resets
  the mixer sink (logged as `mixer_reset`) and restarts each voice/channel
  from the restored HLE state.
- GPU texture caches: dropped. Textures re-decode from restored guest RAM.
- Gap-log dedup sets, draw/gap sinks, frame sinks, flip observers, vblank
  hooks, pacing bases: host closures or derived logs, re-established by the
  host or restarted empty.
- Fast-forward hold/latch and the pacing multiple: host execution policy,
  like the pacing bases. A snapshot never records it and loading one keeps
  whatever the user currently has selected, so a state saved at 3x restores
  at 1x (or the reverse) without touching the guest clock.
- The disc image and the memory-stick directory: session config. A file
  that no longer resolves fails the load (`snapshot_open_failed`,
  `snapshot_bad_config`) instead of loading a half state.

## Save order and quiesce

Save requires a started session and flushes the live CPU into the current
thread first. Each exporter enforces its own quiesce gate: nested Host
waits fail with `snapshot_host_wait`, live interrupts/guest calls fail with
`snapshot_busy`. In-flight GPU draws are derived, not gated: they are
absent from the snapshot (the game re-submits display lists every frame, so
at most one partial frame) and dropped on load; the saver logs the count as
`snapshot_gpu_pending_unsaved`. A parked audio waiter is saveable on purpose:
its drain poll and cancel hook are rebuilt from the channel index on load,
and a marker with no matching thread fails as `snapshot_dangling_wait`.

Audio waits use absolute deadlines and VTimers absolute schedules against
the restored virtual clock, so timers fire at the same virtual time.
Guest-visible wall time (`sceRtcGetCurrentTick`, libc time/gettimeofday) is
`wall_base_us + now_us`: the base is host boot time and is restored from the
snapshot, so ticks stay continuous across a restore in a new host process.
Without this the guest sees a minutes-scale time jump at the resume point and
takes a different branch (found as scattered heap/byte divergence with
identical flips/vblanks; see BRINGUP). Preemption boundaries likewise fire on
a Runtime dispatch counter (`dispatches_since_import_`, every starvation
interval): the saved `dispatch_phase` keeps the post-restore boundary
schedule — and therefore the thread interleaving — identical to the
continuous run.

## Post-failure rule

Byte-level rejections (truncated, hash, version, corpus, config) leave the
session untouched. A semantic failure after commits have started (corrupt
structure that still hashes, which should be unreachable) invalidates the
session: do not continue it, restart from boot or a good snapshot.

## Using it

Host (`boot_runner`, built with the game corpus):

```sh
# save at the end of a run (e.g. stopped at the first battle)
boot_runner --elf ... --nids ... --disc ... --ms ... [run flags...] \
    --snapshot-out build/qa/battle.gsnap
# resume another run from the snapshot
boot_runner --elf ... --nids ... --disc ... --ms ... [run flags...] \
    --snapshot-in build/qa/battle.gsnap
```

The runner logs `snapshot_saved` / `snapshot_loaded` (+ resume PC) and
`mixer_reset` on the diagnostic line; failures exit non-zero with the
named error.

Android: the Menu overlay (SettingsOverlayView) has Save state / Load state /
Reset to title rows driving the same paths through JNI into
`getExternalFilesDir()/snapshots/snapshot.gsnap` (user-facing wording is
"state", never "snapshot").
`adb push` a host snapshot there and load it from the menu:

```sh
adb push build/qa/battle.gsnap /sdcard/Android/data/com.gungnir.recomp/files/snapshots/snapshot.gsnap
adb shell ls /sdcard/Android/data/com.gungnir.recomp/files/snapshots/
```

No `MANAGE_EXTERNAL_STORAGE`, no SAF for this flow. The `snapshot_in`
intent extra loads a snapshot before the session thread starts (automation
only); menu Save stops the worker, saves, and resumes it; menu Load stops
the worker and restarts it from the snapshot; menu Reset to title stops the
worker and reboots the session from the ELF entry. A failed load leaves the
session stopped: restart the app, do not continue.

# ISO-pick → start (installer) validation — M2102K1AC — 2026-11-06

Date: 2026-11-06
Build: uncommitted tree (v1.1 candidate)
APK: `android/app/build/outputs/apk/debug/app-debug.apk`
APK SHA-256: `7d9f986ffa7cfa89…` (full: see `sha256sum` at build time)

Sign-off: **PASS — picking the ISO starts the game (SAF path, manually confirmed).**

Task: Yakumo-style on-device installer (ISO-only start).

## Device

| Field | Value |
|---|---|
| Model | M2102K1AC |
| Android | 14 |
| GPU | Adreno (system driver) |
| Serial | `30c1f958` |

## Steps

1. Fresh uninstall/install; `files/game/` absent.
2. Launch → SAF picker auto-shows (`game_data_unavailable: no_game_data`).
3. User picked the ISO via SAF; app copied to `files/game/imported.iso`,
   verified, prepared `files/game/game.elf`, started the session.
4. ADB-parity replay (ISO pushed to `files/game/imported.iso`, relaunch)
   exercised the same `ensure_runtime_elf` path and reached `guest_frame=480`.

## Expected and actual

| # | Step | Expected | Actual | Pass |
|---|---|---|---|---|
| 1 | Cold launch, no data | Picker auto-shows, named failure, no crash | `game_data_unavailable: no_game_data`, picker activity shown, no Fatal | Pass |
| 2 | SAF ISO pick | Verify → prepare → start, no `installer_failed` | `game_data_ok origin=saf` → `installer_prepared` → `session_start` | Pass |
| 3 | Guest running | Frames present | `guest_frame=1→480`, `cadence flips=510`, audio mixing | Pass |

## Notes

- Prepared `game.elf` on-device is byte-identical to the host intake
  (`pspdecrypt`) output for the same ISO (local harness check, not committed).
- First SAF pick initially hit `session_start_failed: startup_not_ready`
  (dangling `SessionStartup` after the `if` block); fixed by hoisting the
  context to `SDL_main` lifetime and rebuilt. See `docs/BRINGUP.md`.

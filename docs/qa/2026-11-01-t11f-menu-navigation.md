# QA — gamepad navigation for the Menu overlay (T11f)

Date: 2026-11-01
App commit: `6270e20` plus the T11f working-tree change (the APK was built
from that tree; this report is committed with the change).
APK: debug, `com.gungnir.recomp`.
Sign-off: **PASS for the on-device navigation cases.** Save state was skipped
at the user's request to preserve their snapshot; the restore path was proven
first and the row shares the activation path with the cases that passed.

## Devices

| Device | SoC / GPU | ABI | Android | Role |
|---|---|---|---|---|
| Xiaomi M2102K1AC (Mi 11 Ultra) | Snapdragon / Adreno | arm64-v8a | 14 (API 34) | primary phone |
| 8BitDo Lite 2 | Bluetooth gamepad | - | - | pad under test |

The pad reported as `8BitDo Lite 2`, GUID
`0500821ac82d000012510000ffff3f00`, profile `default`. Screenshots and the
captured action log are in gitignored `build/qa/2026-11-01-t11f/`.

## 1 — Open and close

`SDL/APP` logged `gamepad_connected name=8BitDo Lite 2 ... profile=default`.
Pressing Back opened the overlay and logged:

```text
menu_action=1 index=0
```

The overlay appeared with the first row (`Edit touch layout`) highlighted
(`t11f-open.png`). Pressing Cross on `Close` (index 8) closed it.

Expected: the menu button toggles the overlay and the first row is selected.
Actual: pass.

## 2 — Every row, clamp and scroll

D-pad down produced indices 1 through 8 with no gaps:

```text
menu_action=2 index=1 .. menu_action=2 index=8
```

Two further downs stayed at index 8 (clamp at the last row), and the ScrollView
kept the row reachable. D-pad up returned 8 through 0. This is the fix for the
earlier report that presses were thinned: the UI poll now drains every queued
press per frame instead of one.

Expected: every press moves once, clamped at both ends.
Actual: pass.

## 3 — Activation through the Listener path

From the pad:

| Row | Index | Result |
|---|---|---|
| Close | 8 | overlay closed |
| Reset to title | 7 | session restart path (user-confirmed) |
| Load state | 6 | state load; **no game action fell through** (user-confirmed) |
| Save state | 5 | **skipped** at the user's request; see section 5 |

Expected: Cross runs the same action as a touch tap.
Actual: pass.

## 4 — Suppression

Before this fix, every menu press was also published to the guest as its PSP
mapping, so activating `Load state` could act in the game. The adapter now
reserves Back/Guide for the menu and withholds d-pad and South/Cross while the
overlay is open; gameplay input returns on close. `sdl_gamepad_tests` covers
the reservation and the suppression, including the return to gameplay.

Expected: menu presses do not play the game.
Actual: pass, user-confirmed on device.

## 5 — Snapshot safety (Save state skipped)

The Save row writes the fixed `snapshot.gsnap` path, so the user declined to
overwrite their state. Before the decision, the existing file was pulled,
pushed back and re-hashed on the device; the round trip was byte-identical
(`build/qa/2026-11-01-t11f/snapshot-restore.txt`). The Save row's activation
path is the same `Listener` call as `Load state`, which passed. Recorded as a
deliberate gap, not a failure.

## 6 — Touch path

The on-screen Menu button was tapped with `adb shell input tap`; the overlay
opened with row 0 highlighted, and tapping `Gamepad` opened the info dialog
(`8BitDo Lite 2`, GUID, `profile: default`). Android Back dismissed it
(`t11f-touch-gamepad.png`). Touch handlers are unchanged; the only addition is
the selection highlight.

Expected: touch still opens and activates rows.
Actual: pass.

## Known limits

- One action per press: a held d-pad does not auto-repeat.
- Back/Guide are reserved for the menu, so Select cannot be bound to either;
  binding Select to another button works.
- The pending queue holds eight presses; a UI stall longer than that drops the
  oldest.

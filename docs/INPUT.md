# INPUT — touch, gamepad, keyboard and touch haptics (T11)

`runtime/input/` owns a source-neutral state. Touch, gamepad and desktop
keyboard/mouse producers publish buttons and one analog pair; `sceCtrl`
reads the merged view. The Android shell draws the touch controls in a
native `View` above the SDL surface and forwards touches through JNI.

## Source merge

Slots are keyed by `(source, device, pointer)`:

| Source | Producer |
|---|---|
| Touch | `TouchOverlayView` via JNI; one merged touch mask/analog |
| Gamepad | `SdlGamepad`; one slot per SDL instance id |
| Keyboard | desktop/Android key events through `SdlInput` |
| Mouse | mouse buttons through `SdlInput` |

- Buttons are OR-ed. A button is held while any source slot holds it; one
  source releasing does not release a button another source still holds.
- Analog uses the highest-priority engaged slot in the fixed order Touch,
  Gamepad, Keyboard, Mouse. Within a source the most recently updated pointer
  wins. A centred touch stick exposes the gamepad underneath.
- Axes are clamped to `[-1, 1]`; the 0.12 dead zone is applied to the merged
  value, then the PSP byte is `0x80 + value * 127.5` clamped to `0..255`.
- Make/break edges are accumulated for the host and consumed by
  `take_edges()`. The sceCtrl latch keeps its own previous-button copy.
- `InputState` is mutex-protected because the Android `View` runs on the UI
  thread while HLE reads on the SDL thread.
- Focus loss/background releases every source; gamepad disconnect releases
  its slot and republishes neutral.

`sceCtrlReadBufferPositive`/`PeekBufferPositive` publish the merged
`SceCtrlData` (timestamp, buttons, Lx/Ly/Rx/Ry). The make/break/press/
release latch samples on positive-buffer reads; `sceCtrlReadLatch` returns
and resets it, `sceCtrlPeekLatch` returns it without resetting.

## Touch overlay

`TouchOverlayView` draws a pixel snapshot from
`runtime/input/touch_overlay.*` and forwards `MotionEvent`s by pointer id.
Controls: floating analog stick, 8-way d-pad, Cross/Circle/Square/Triangle,
L/R, START/SELECT and FF (the fast-forward toggle, left of SELECT).

- The stick appears where a finger lands in the left half (outside placed
  controls) and its origin follows when the thumb passes the reach.
- A finger keeps its pointer id and role, so sliding from one face button to
  another releases the first and presses the second without sticky state.
- `ACTION_CANCEL`, focus loss and background release every pointer.
- Positions are normalized to the safe content box; radius scales with the
  shorter content dimension. The View receives system-bar/cutout insets.

`MenuButtonView` is the on-screen `MENU` ring that opens Settings. It is
painted like START/SEL (same stroke, radius and caption colours) and hides
with the rest of the touch chrome while a gamepad with a Guide button is
active; Guide opens the same surface from the pad. A pad without a Guide
button keeps the ring, since nothing else opens Settings from it. While
Settings is open the ring is `GONE`, and closing it restores whichever state
the gamepad poll reports.

### Editing

`Menu > Edit touch layout` enters edit mode; the same row becomes
`Finish touch layout`. Drag a control to move it, drag its yellow corner
handle to resize; both clamp inside the safe box. `Menu > Reset touch
layout` restores the default after a confirmation. `Back` finishes editing
or closes Settings first.

The layout is versioned JSON in app-private storage:

```json
{"version":1,"controls":[{"control":"cross","cx":0.82,"cy":0.74,
 "radius":0.05,"visible":true}, ...]}
```

`touch_layout_store.*` writes a sibling `.tmp` file and renames it over the
target, leaving the previous file intact on failure. A missing file logs
`touch_layout_default_missing`; an older schema logs
`touch_layout_older_default`; malformed JSON or a missing/duplicate control
logs `touch_layout_corrupt_default`. All fall back to the default and never
crash the overlay. A save failure logs `touch_layout_save_failed`.

## Gamepad

`SdlGamepad` initializes `SDL_INIT_GAMEPAD`, opens a pad on
`SDL_EVENT_GAMEPAD_ADDED`, publishes buttons/analog as the Gamepad source,
and releases everything on `SDL_EVENT_GAMEPAD_REMOVED`, background or
shutdown. The touch overlay hides while a pad is connected and returns on
disconnect (400 ms Java poll of `GamepadBridge.active()`).

Profiles are pure data. Buttons are indexed by `SDL_GamepadButton`; the
left stick uses `SDL_GamepadAxis` X/Y. The mapping is: South/Cross,
East/Circle, West/Square, North/Triangle, Back/Select, Start/Start,
shoulders L/R, d-pad Up/Down/Left/Right. The right stick click (R3) and the
right trigger (R2) are not PSP buttons: while a profile leaves them unbound
they drive fast-forward instead of the guest (see below).

- Shipped profiles live in
  `android/app/src/main/assets/input_profiles/profiles.json` and are matched
  by case-insensitive device-name substring: `default` (fallback `*`), Xbox,
  DualSense, PlayStation Wireless (`Wireless Controller`, after DualSense so
  it does not steal it), Switch Pro Controller, Joy-Con, 8BitDo, Razer Kishi,
  Backbone One, GameSir, Flydigi, PowerA, `Ayn Odin`, `AYANEO`.
  An unknown name uses `default`. All shipped entries share the standard
  layout; they differ by dead zone (Joy-Con 0.15, Flydigi 0.10, most 0.12).
- User remaps save a versioned per-GUID profile at
  `files/input_profiles/<guid>.json`. On connect the user profile wins; a
  corrupt/older/same-axis file logs `gamepad_profile_fallback: ...` and falls
  back to the shipped profile.
- Open failure logs `gamepad_open_failed: ...`; connect/remove log
  `gamepad_connected`/`gamepad_removed` with name, GUID and profile id.
- A connected pad whose GUID has no SDL mapping and no saved user remap
  cannot work: its buttons arrive at raw, non-standard indices that no
  shipped profile listens to. The touch overlay toasts once per connection
  ("Controller not set up — remap it in Menu > Gamepad"); the remap wizard
  records raw indices, so it covers unmapped pads. The deeper fix — a
  `gamecontrollerdb.txt` SDL-mapping layer like upstream Yakumo's
  `profiles/mhp3rd/host/input/gamepad_devices.cpp` (user-editable mapping
  file loaded at startup) — does not exist here yet; shipped profiles only
  tune dead zones over SDL standard positions and cannot substitute for it.

### Remap

`Menu > Gamepad` shows name, GUID and profile. `Remap` opens a non-focusable
dialog so the SDL surface keeps receiving gamepad events. The wizard captures
twelve buttons (Cross, Circle, Square, Triangle, L, R, Start, Select, d-pad
Up/Down/Left/Right), then:

```text
Push the left stick left/right (13/14)
Push the left stick up/down (14/14)
```

The X and Y axes must be distinct; reusing the X axis for Y is rejected, and
a saved profile with equal axes is corrupt. `Save` writes the per-GUID file
and uses it immediately; `Reset profile` restores the default. Save failures
are returned to the dialog.

## Fast-forward

Fast-forward is host execution policy, not guest input. `HostControls`
(`runtime/input/host_controls.*`) owns a hold and a latch per control:

- **hold** — momentary: the R2 trigger (axis over half). Releasing returns
  to 1x.
- **latch** — a toggle: the FF touch button and a press of R3 (right-stick
  click). One press latches, the next clears, and the release changes
  nothing.

`active()` is hold OR latch, so a held trigger and a latched button never
fight; `release_holds()` on background/focus loss drops the momentary source
and leaves the latch (the user's choice survives suspend/resume).
`speed()` is 1.0 idle and `kFastForwardSpeed` (3.0) active.

The control never reaches `sceCtrl`: FF is not a PSP button, so a tap on the
FF ring publishes no guest bit and a held R2/R3 changes no pad byte. The
kernel's pacing hold consumes the speed (see `HLE.md`), the mixer mutes while
it is active (see `AUDIO.md`), and the Android shell lights the FF ring for
any source and toasts `Fast-forward 3x` / `Fast-forward off` on each change
(the ring is the persistent state; the toast is only feedback, as save/load
do).

Rules that keep a binding from being stolen:

- R3 only toggles fast-forward while the connected profile leaves
  `SDL_GAMEPAD_BUTTON_RIGHT_STICK` unbound. A user remap that binds R3 to a
  PSP button wins, and the wizard never maps R2 or R3.
- The menu overlay is modal: neither R2 nor R3 acts on the game while
  Settings is open.
- Desktop keyboard/mouse input has no fast-forward binding; it is a pad and
  touch feature.

## Keyboard and mouse (desktop development)

`KeyboardMap` uses SDL scancode positions, so W A S D stay in place on any
layout:

| Input | PSP |
|---|---|
| W / Up, S / Down, A / Left, D / Right | d-pad |
| J, K, L, I | Cross, Circle, Square, Triangle |
| Q, E | L, R |
| Return, Backspace | Start, Select |
| mouse left / right / middle | Cross / Circle / Square |

`SdlInput` feeds key/mouse events into `InputState`; repeats are ignored and
focus loss releases held keys. Touch and gamepad are the device paths.
Synthetic mouse events from touches (`SDL_TOUCH_MOUSEID`) are dropped — the
touch overlay owns touch input, and without the filter a tap that falls
through to the SDL surface maps to Cross (Android additionally disables the
synthesis with `SDL_HINT_TOUCH_MOUSE_EVENTS=0`). A fresh `AC_BACK` key press
toggles the settings overlay instead of reaching the guest.

## Touch haptics

Only a new face-button, L/R, START/SELECT or d-pad touch-down requests a
tick. The stick, slide/move, edit drag/resize and release are silent.

`TouchOverlayView` checks `Settings.System.HAPTIC_FEEDBACK_ENABLED` and calls
`performHapticFeedback(HapticFeedbackConstants.CLOCK_TICK)`; disabled
haptics logs `touch_haptic_disabled`, a performed tick logs
`touch_haptic performed=true`, and a failure logs `touch_haptic_failed` without
blocking input. There is no time-based throttle: five rapid downs produce
five requests, and Android coalesces at the hardware layer. A missing
vibrator or absent sink is a no-op.

Gamepad rumble, `sceRumble` and battle haptics are not driven by T11; they
remain T13/M5.

## Menu navigation

The settings overlay is operable from a paired pad (T11f) and from the
system back key. Guide or system back toggles it; d-pad up/down moves the
highlighted row and South/Cross activates it through the same Listener path
as a tap. The selection clamps at the first and last row (it does not wrap),
so "Reset to title" and "Close" cannot be reached by overshooting. The
system-back path exists because SDL consumes `KEYCODE_BACK` and delivers it
as an `AC_BACK` key (the activity back dispatcher never fires); `SdlInput`
routes a fresh `AC_BACK` press to the menu toggle and the guest map drops it.
On Android 15 the `OnBackInvokedDispatcher` callback remains as the gesture
path.

`runtime/input/menu_navigation.{hpp,cpp}` owns the state machine and is
SDL/Java-free: SDL button-down edges in `sdl_gamepad.cpp` call `press()`, and
the UI thread drains `take()` through `nativeMenuPoll()` on a frame callback
while the activity is resumed. `nativeMenuBegin(row_count)` on show and
`nativeMenuEnd()` on hide reset the selection to the first row. No native
callback into Java is needed. The poll drains every queued press, so a fast
d-pad roll is not thinned out.

Guide (SDL `GUIDE`, Home) is the menu button and never reaches the guest.
Select (SDL `BACK`) is a normal guest button and stays remappable; the remap
wizard captures it because the wizard runs with the overlay closed. While the
overlay is open no pad button reaches the guest: Guide toggles it, d-pad
up/down move the selection, South activates, and the remaining face, shoulder,
Start and Select buttons are withheld, so navigating the menu never plays the
game. Gameplay input returns as soon as it closes. This matches the touch
overlay, which is also suppressed while the settings are open.

## Tests

```sh
python3 -m unittest tests.test_input -v
python3 -m unittest tests.test_touch_overlay -v
python3 -m unittest tests.test_touch_haptics -v
python3 -m unittest tests.test_gamepad -v
python3 -m unittest tests.test_sdl_gamepad -v
python3 -m unittest tests.test_sdl_input -v
python3 -m unittest tests.test_host_controls -v
python3 -m unittest tests.test_menu_navigation -v
python3 -m unittest tests.test_input_suite -v
```

`tests.test_sdl_gamepad` attaches a virtual SDL joystick and skips with a
named reason when the host has no SDL3 pkg-config. The physical acceptance
device remains the arm64 phone; see
`docs/qa/2026-10-31-t11b-touch-overlay.md`,
`docs/qa/2026-10-31-t11c-touch-haptics.md` and
`docs/qa/2026-10-31-t11d-gamepad.md`.

## Named gaps

- Game/synthetic-fixture navigation is T13. T11 verifies the input path,
  overlay behavior and haptics, not a game response.
- Ayn/AYANEO built-in controllers were not available for hardware QA; the
  shipped profiles are name-matched and fall back to `default`.
- The x86_64 emulator exposes a virtual vibrator, so it exercises the tick
  path; no-vibrator behavior is covered by the absent-sink host contract and
  `performHapticFeedback` returning false.
- Gamepad rumble and battle haptics are T13/M5.
- Fast-forward audio is dropped, not time-stretched: the mixer counts
  `muted_buffers` and restarts its timeline on unmute. A pitch-preserving
  stretcher does not exist.
- Fast-forward has one speed (3x, `kFastForwardSpeed`); a configurable
  multiple is not exposed.
- Menu navigation is one action per press: a held d-pad does not repeat.
  Every queued press is applied on the next frame; the queue holds eight, so a
  UI stall longer than about eight presses can still drop the oldest.
- Guide is the menu button and is never forwarded to the guest, so a profile
  that wants a guest action on SDL's `GUIDE` index cannot have it. Select
  (`BACK`) is a normal, remappable guest button. A pad without a Guide button
  keeps the on-screen Menu ring, because nothing else opens Settings from it.

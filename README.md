# gungnir-recomp

![project-banner.jpg](project-banner.jpg)

Static recompilation of **Gungnir: Inferno of the Demon Lance and the War of
Heroes** (PSP, USA — ULUS-10592) to native C++, targeting **Android**.

**Bring your own game.** This repository contains no game data, no game code
(including no generated/translated game code), no keys, no firmware, and no
driver binaries.

## Features

| Feature | State | Notes |
|---|---|---|
| Title screen + menu on device | Working | Touch and gamepad verified into real battles (M4) |
| Touch overlay | Working | Floating stick, d-pad, face buttons, editable drag/resize layout, touch haptics |
| Gamepad | Working | Per-device profiles, in-menu remap wizard; overlay hides while a pad is active |
| Fast-forward 3x | Working | R2 hold, R3 latch, FF touch ring; latched state survives suspend/resume |
| Memstick saves | Working | Save/load round trip verified on device (`docs/SAVEDATA.md`) |
| Host save states | Working | Menu-driven snapshot save/restore (`files/snapshots/`) |
| Suspend/resume | Working | Session stops between dispatches, resumes in place |
| Adaptive widescreen | Working | `adaptive` presentation with coverage diagnostic; per-title camera hook pending |
| Custom Adreno drivers | Working | User-supplied ICD zip, fail-closed (`docs/ANDROID_DRIVERS.md`) |
| Texture dump/replace | Working at 1x | Dump hash-keyed PNGs, user PNG replacement (`docs/TEXTURE_REPLACEMENT.md`); Nx upscaling pending |
| Experimental frame interpolation | Experimental | Toggle in Settings; wall/mesh pairing artifacts under investigation |
| Audio (SAS/ATRAC) | Partial | Music/SFX play; battle underruns and full-scale peaks known-bad (M5) |
| Rumble | N/A | The game imports no rumble/actuator API, so there is nothing to wire (T14b audit) |

## Status

M4 (title screen on adb device) is **closed**: title, menus, and real gameplay
into battles verified on device, including an in-game save/load round trip;
T13 (integration) and T14 (device QA + sign-off) are closed with dated
reports in [`docs/qa/`](docs/qa/). Master plan: [`.dex/plan.md`](.dex/plan.md)
— tasks T1–T18, milestones M0–M5. Current focus: M5 hardening and the
remaining defect tracks (texture seams, frame-interpolation pairing).

| Milestone | Meaning | State |
|---|---|---|
| M0 | Repo live, IP policy enforced | **closed** (T1+T2+T3; ledger: `data/milestones.json`) |
| M1 | Recomp pipeline proven on synthetic fixtures (Linux) | **closed** (T4; `data/milestones.json`) |
| M2 | Gungnir analyzed, AOT generated locally | **closed** (T5; `data/milestones.json`) |
| M3 | Bounded boot on Linux desktop | **closed** (T6+T7; ledger: `data/milestones.json`) |
| M4 | **Title screen on adb device** | **closed** (T13+T14; title, menu, gameplay, save/load, soak evidenced in `docs/qa/`) |
| M5 | Hardening: audio, saves, fast-forward, texture upscaling, driver picker, perf | **planned** (fast-forward, memstick saves, host snapshots and driver picker in; battle-audio quality and texture work ongoing) |

Analysis details and the current census: [`docs/ANALYSIS.md`](docs/ANALYSIS.md);
bring-up findings journal: [`docs/BRINGUP.md`](docs/BRINGUP.md); public
status and release checklist: [`docs/PROGRESS.md`](docs/PROGRESS.md),
[`docs/RELEASE.md`](docs/RELEASE.md).

## Controller mapping

Sources merge (buttons OR-ed, analog by priority touch → gamepad → keyboard
→ mouse); see [`docs/INPUT.md`](docs/INPUT.md). No gamecontrollerdb.txt
layer yet — unmapped pads fall back to the in-menu remap wizard.

| PSP input | Touch overlay | Gamepad | Desktop keyboard/mouse |
|---|---|---|---|
| Cross / Circle / Square / Triangle | Face buttons | South / East / West / North | J / K / L / I; mouse left / right / middle = Cross / Circle / Square |
| D-pad | 8-way d-pad | D-pad | W / Up, S / Down, A / Left, D / Right |
| L / R | L / R buttons | Shoulder buttons | Q / E |
| Start / Select | START / SELECT | Start / Back | Return / Backspace |
| Analog stick | Floating stick (left half) | Left stick | — (pad and touch feature) |
| Fast-forward (host, not a PSP button) | FF ring (toggle) | R2 hold, R3 toggle (while unbound in profile) | — |
| MENU ring / Settings | MENU ring | Guide button | — |
| Rumble | — (touch haptics only) | — (game has no rumble API) | — |

## Custom Adreno drivers

Gungnir can load a user-supplied Mesa Turnip/Qualcomm Vulkan ICD package on
arm64 Android through libadrenotools. See [`docs/ANDROID_DRIVERS.md`](docs/ANDROID_DRIVERS.md).

## Building

The full from-scratch flow — intake, decryption, analysis, codegen, host
tests, Android build and device run — is in
[`docs/BUILDING.md`](docs/BUILDING.md). Android shell setup and deploy are in
[`docs/ANDROID.md`](docs/ANDROID.md)

**Have a `.chd`?** The pipeline's input is a UMD **ISO** — converters, the
identity gate, and the full intake flow live in
[`docs/INTAKE.md`](docs/INTAKE.md). `game.chd`/`game.iso` are gitignored and
never committed.

## License

Project-original code: [GPL-3.0-or-later](LICENSE). Third-party components
are pinned, digest-verified and attributed (`dependencies.json`, the
generated `THIRD_PARTY_NOTICES.txt`, `docs/SOURCE_PROVENANCE.md`); vendored
code carries its license in-tree (`third_party/*/LICENSE`).

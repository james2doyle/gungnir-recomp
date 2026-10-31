# gungnir-recomp

Static recompilation of **Gungnir: Inferno of the Demon Lance and the War of
Heroes** (PSP, USA — ULUS-10592) to native C++, targeting **Android**
(arm64-v8a + x86_64). The game's Allegrex machine code is translated
ahead-of-time to C++, compiled natively, and executed against an HLE
reimplementation of PSP system software with a Vulkan renderer.

**Bring your own game.** This repository contains no game data, no game code
(including no generated/translated game code), no keys, no firmware, and no
driver binaries — by design and by enforcement (see below). You supply your
own legally obtained UMD ISO (`.chd` dumps are converted first — see
[`docs/INTAKE.md`](docs/INTAKE.md)); the pipeline verifies its identity,
generates recompiled code locally into a gitignored `build/` tree, and the
result runs on your device.

> Not affiliated with or endorsed by Sting Co., Ltd. or Atlus U.S.A., Inc.
> PSP/PlayStation are trademarks of Sony Interactive Entertainment Inc.

## Status

Early development. Master plan: [`.dex/plan.md`](.dex/plan.md) — tasks T1–T13,
milestones M0–M5. The first milestone (M4) is the game's **title screen
rendered on an adb-connected device with navigable menus** (touch + gamepad).

| Milestone | Meaning | State |
|---|---|---|
| M0 | Repo live, IP policy enforced | **closed** (T1+T2+T3; ledger: `data/milestones.json`) |
| M1 | Recomp pipeline proven on synthetic fixtures (Linux) | **closed** (T4; `data/milestones.json`) |
| M2 | Gungnir analyzed, AOT generated locally | **closed** (T5; `data/milestones.json`) |
| M3 | Bounded boot on Linux desktop | **closed** (T6+T7; ledger: `data/milestones.json`) |
| M4 | **Title screen on adb device** | planned |
| M5 | Hardening: audio, saves, texture upscaling, driver picker, perf | planned |

Analysis details and the current census: [`docs/ANALYSIS.md`](docs/ANALYSIS.md);
bring-up findings journal: [`docs/BRINGUP.md`](docs/BRINGUP.md).

## Hard constraints

- **No proprietary content is ever committed** — the full charter is
  [`docs/LEGAL.md`](docs/LEGAL.md). Enforced structurally:
  `.gitignore` + [`tools/repository_check.py`](tools/repository_check.py)
  (extension/magic-byte/size/secret/ignore-probe audit) + CI on synthetic
  fixtures only. `AGENTS.md` is the policy authority for contributors.
- **Decryption is out-of-process** (you run `pspdecrypt`/PPSSPP devtools on
  your own copy); this project ships no decryption code and no keys.
- **Red-first testing** (plan §8.1): tests land before implementations,
  fixes start from reproducing reds, nothing closes red. Device-facing work
  additionally requires **recorded manual QA** on real hardware (plan §8.3).

## Repository layout

```
tools/       intake (iso → verified game tree), analysis, codegen glue,
             format tools, repository_check.py (the IP gate)
runtime/     guest memory, dispatch + interpreter fallback, HLE kernel,
             GE/Vulkan renderer, post-processing, input, audio, savedata
profiles/gungnir/   per-game config (facts vs. choices); local/ is gitignored
android/     Gradle + JNI application shell (T9; SDL3 Activity, Vulkan surface)
tests/       synthetic fixtures + gate tests (our authorship only)
docs/        legal charter, provenance, format specs, bring-up journal
.dex/        plan + task tracker (dex, in-repo)
build/       GITIGNORED: your game tree, generated recompiled code, outputs
```

## Building

Android shell setup, build/deploy and SAF/adb intake are in
[`docs/ANDROID.md`](docs/ANDROID.md). See `docs/BUILDING.md` (landing with
milestone M4) for the full from-scratch flow. Prerequisites:
`cmake`, `ninja`, a C++20 compiler, Python 3.11+, Android SDK/NDK (arm64),
and your own disc image.

**Have a `.chd`?** The pipeline's input is a UMD **ISO** — converters, the
identity gate, and the full intake flow live in
[`docs/INTAKE.md`](docs/INTAKE.md). `game.chd`/`game.iso` are gitignored and
never committed.

## License

Project-original code: [GPL-3.0-or-later](LICENSE). Third-party components
are pinned, digest-verified and attributed (`dependencies.json`, the
generated `THIRD_PARTY_NOTICES.txt`, `docs/SOURCE_PROVENANCE.md`); vendored
code carries its license in-tree (`third_party/*/LICENSE`).

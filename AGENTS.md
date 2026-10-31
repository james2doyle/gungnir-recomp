# AGENTS.md — policy authority for gungnir-recomp

This file is the single policy authority for all human and AI contributors.
If another document conflicts with this one, this one wins.

## What this project is

Static recompilation of *Gungnir* (USA, ULUS-10592) for PSP to native C++,
targeting Android. See `.dex/plan.md` for the master plan and `README.md` for
orientation. Work is tracked as dex tasks in `.dex/tasks.jsonl` (in-repo).

## Hard limits (never relaxed)

0. **Never write a clean-room reimplementation of an existing format, codec,
   or protocol without explicit user sign-off.** If a task reduces to
   "implement X that already exists" (a container parser, a codec, a
   well-specified file format), **stop and ask**: prefer using the existing
   tool or library (out-of-process if its license demands isolation), or get
   approval before reimplementing. Example: `game.chd` → ISO uses CHDStudio
   or `chdman`, not a hand-written CHD decoder.

1. **No proprietary content, code, or work is ever committed.** No game
   disc images (ISO/CHD/CSO), no EBOOT/BOOT/ELF/PRX (encrypted or decrypted),
   no game assets (DAR contents, TPL textures, VAG/AT3 audio, PGF fonts,
   script `.bin`), no game text (original or translated), no game screenshots
   or footage, no Sony firmware, no KIRK/DRM key material, no Adreno driver
   binaries, and **no generated recompilation output** (AOT C/C++ is a
   derivative of the copyrighted executable — it is build output, never
   source). The full charter: `docs/LEGAL.md`.
2. **No keys or credentials** of any kind (PSP key files, keystores, API
   tokens) — not even samples or "test" values. Format placeholders live in
   docs as clearly fake text only.
3. **No hidden failures.** No loop caps that quietly stop, no sleeps that mask
   races, no forced returns, no fake-success stubs, no silently disabled
   tests. When something is unknown or unsupported, it fails **loudly** with a
   named error (e.g. `NID_0x…` traps, `missing_function=stop`) or is recorded
   as an explicit, counted gap — never papered over.
4. **Generated code is never committed and never hand-edited.** If generated
   output is wrong, fix the generator. The user generates it locally from
   their own disc.
5. **No dump identities in public files.** SHA-256 pins of the game's EBOOT
   belong in gitignored `profiles/gungnir/local/`, not in tracked TOML.
   Published game facts are limited to: disc identity (size + CRC32, recorded
   by redump), format layouts, NID values, guest addresses/record sizes.
6. **PPSSPP is a diff oracle, never a link target** — and no emulator code is
   vendored into the product.

## Testing discipline (red-first)

Full rules: `.dex/plan.md` §8.1. Summary:

- Write the failing test **first**; observe it fail **for the expected
  reason**; then implement. Commit the red test alongside its implementation.
- Bug fixes start with a test that reproduces the bug against current code.
- No task closes with a red or disabled test case; disabled tests need an
  itemized, time-boxed TODO.
- Semantic changes to codegen/HLE get a mutation check: deliberately break one
  semantic and confirm the suite dies for that reason.
- Every binary writer (DAR repack, TPL re-encode, font atlas, PARAM.SFO) must
  prove a byte-identical round-trip before it may write anything new.
- Bounded runs everywhere (dispatch + wall-clock limits on guest execution);
  synthetic runs are never reported as game runs. Evidence in
  `docs/BRINGUP.md` is labeled VERIFIED / HYPOTHESIS / PLANNED.

## Device validation

Automated tests never close a device-facing milestone. Manual QA on adb
devices is mandatory, structured, and recorded (device matrix, build SHA,
steps, expected/actual, sign-off) per `.dex/plan.md` §8.3 and `docs/QA.md`.
"CI is green" is not evidence; three consecutive cold boots are.

### Emulator vs physical device

The emulator is the right tool during the initial phases of a feature:
fast iteration, no device claim, quick UI/logcat checks. It does **not**
close a feature sprint. At the end of every device-facing feature sprint,
run the manual QA on a physical device (currently the arm64 Adreno phone);
emulator runs may supplement that evidence but never replace it. Record
the physical device model, ABI/API, GPU/driver, build SHA, steps,
expected/actual and sign-off in `docs/qa/` per `.dex/plan.md` §8.3.

Use `scripts/android/emulator.sh` for a reproducible run. It uses the
`gungnir` AVD on port `5554` and the same flags every time:

```sh
ANDROID_AVD_HOME=${ANDROID_AVD_HOME:-$HOME/.config/.android/avd} \
  scripts/android/emulator.sh
# exact emulator command:
# $ANDROID_HOME/emulator/emulator -avd gungnir -port 5554 \
#   -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect \
#   -no-snapshot -wipe-data -netdelay none -netspeed full
```

The script waits for `sys.boot_completed=1` and prints
`emulator ready: emulator-5554`. `-no-window` means headless: interact
through `adb`, `uiautomator`, and `scripts/android/screenshot.sh`.
`-wipe-data` deliberately erases emulator state for a clean boot; re-push
the user ISO and any driver fixtures afterwards. Use `ADB_SERIAL` to
target the emulator or the phone explicitly.

## Third-party code

- License must be GPL-3.0-compatible (MIT/BSD/Apache-2.0/LGPL/GPL-2.0-or-later
  OK with attribution and notices; CC BY-NC-SA and unattributed code are
  rejected). Process: `docs/SOURCE_PROVENANCE.md`.
- Pin exact upstream commit in `dependencies.json`; build from source; never
  commit third-party binaries.
- Reimplement behavior, don't copy from incompatible licenses. Inspiration is
  recorded as prose, not lineage.

## Repository hygiene

- `tools/repository_check.py` must pass before every commit and in CI. The
  `.gitignore` is the first line of defense; the gate is the second; both
  have regression tests in `tests/`.
- Commits by AI agents end with `Assisted-by: <tool> (<model>)` (never
  `Co-Authored-By`). Never reference dex task IDs in commits, PRs, or docs —
  describe the work itself.
- Docs obey the charter: format facts and RE addresses yes; game content,
  dump hashes, screenshots no.

## Work tracking

- Plan: `.dex/plan.md` (tasks T1–T13, milestones M0–M5, gotchas register).
- Tasks: dex (`dex list`, `dex start <id>`, `dex complete <id> --result …`).
  Keep the list current as work proceeds.
- `docs/BRINGUP.md` is the living findings journal (symptom → falsification →
  root cause → regression test; log retractions).
- **Reference discovery (every task):** before implementing, search code for
  the relevant API or symbol (`gh search code "<api-or-symbol>"`, adding
  `--extension cpp` for C/C++ HLE work) to find concrete implementations and
  their projects; follow with `gh search issues "<api-or-symbol>"` for
  semantics and regressions. Record the references used.

# Gungnir Recomp — release checklist

Every box is checked against the tree and the scripts named below. Paths
are repository-relative; no machine-local paths, no `local.properties`
(the file is never committed and never required — see `docs/BUILDING.md`).

## 1. Clean clone

- [ ] Fresh `git clone` plus documented submodule/dependency fetch only.
- [ ] `python3 tools/deps.py verify` passes (pinned `dependencies.json`).
- [ ] No game data present: `python3 tools/repository_check.py` PASS.
- [ ] No `local.properties`, keystores, or API tokens anywhere in the tree.

## 2. Notices, licenses, REUSE

- [ ] `python3 tools/package_notices.py generate` is current;
      `THIRD_PARTY_NOTICES.txt` covers every file under `third_party/`.
- [ ] License gate passes (GPL-3.0-compatible only; CC BY-NC-SA rejected).
- [ ] SPDX/REUSE lint passes on all tracked files.

## 3. CI

- [ ] Linux runtime job green on synthetic fixtures only.
- [ ] Android cross-compile job green (see `scripts/android/build.sh`).
- [ ] `repository_check` + secret scan are merge-blocking, not advisory.
- [ ] Private-input tests exit 77 (fail-closed skip), never pass silently.

## 4. APK build

- [ ] `scripts/android/build.sh` produces the APK from a clean checkout
      (machine paths only via documented `-P` properties).
- [ ] `scripts/android/install.sh` + `scripts/android/run.sh` deploy and
      launch on the adb device; `scripts/android/screenshot.sh` captures
      to gitignored `build/qa/` (hashes recorded, images never committed).
- [ ] Emulator smoke via `scripts/android/emulator.sh` before device runs.
- [ ] Release APKs are `assembleRelease` signed with the stable maintainer
      cert: repository secrets `ANDROID_KEYSTORE_BASE64` (+
      `ANDROID_KEYSTORE_PASSWORD` / `ANDROID_KEY_ALIAS` /
      `ANDROID_KEY_PASSWORD`) decoded to a temp `.jks` in
      `.github/workflows/release.yml`; `build.sh --release` fails
      `missing_signing` without it, never falls back to an ephemeral
      debug key. Debug builds keep the default debug signature. No
      keystore, password, or base64 blob is ever committed (see
      `.gitignore` + `tools/repository_check.py`).
- [ ] No generated recomp output and no game content in the APK sources
      or the release artifacts (derivative of the copyrighted executable;
      built locally from the user's own disc).

## 5. User corpus and decrypt steps

- [ ] Release notes point at `docs/BUILDING.md` §3 (disc intake) and §4
      (analysis/codegen) as the only supported corpus path.
- [ ] Host intake decryption stays out of process (`pspdecrypt` on `PATH`
      or `--decrypt-tool`) for codegen; the runtime installer decrypts
      on-device from the user's own ISO with the public PSP key tables
      (see `docs/LEGAL.md`). No game-derived bytes are shipped either way.
- [ ] Identity gate (recorded size/CRC32) refuses a wrong image with a
      named failure before anything else runs.
- [ ] Game data travels via the documented push path only
      (`scripts/android/deploy.sh`); saves live under the app's
      `getExternalFilesDir()` storage, never `MANAGE_EXTERNAL_STORAGE`.

## 6. Versioning and contents

- [ ] `versionName` is the tag without `v` (`v1.3.0` → `1.3.0`);
      `versionCode` is `MAJOR*1000000 + MINOR*1000 + PATCH` derived from
      the tag in `release.yml`, strictly increasing so each release
      installs over the previous one. Local/debug builds keep `1`/`0.1.0`.
- [ ] Version and git SHA recorded in the release notes and the milestone
      ledger (`data/milestones.json` M4 evidence pointers).
- [ ] Artifact list: APK, `THIRD_PARTY_NOTICES.txt`, SHA-256SUMS of the
      artifacts, and the matching `docs/qa/` session reports by date.
- [ ] Public files carry counts and criteria only: no game screenshots,
      dump hashes, keys, or generated code.

## 7. Sign-off

- [ ] Full Python suite green (`tools/run_python_suite.sh`).
- [ ] `tools/repository_check.py` PASS on the release tree.
- [ ] QA matrix signed off per `docs/QA.md` (T14c owns the sign-off);
      the milestone ledger is updated with measures and evidence pointers.

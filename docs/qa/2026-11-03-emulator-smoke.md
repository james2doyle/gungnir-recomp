# QA — emulator smoke (T14a matrix)

Date: 2026-11-03
Build: `c93e2b1` (clean tree; x86_64 ABI + AOT corpus build variant)
APK: `android/app/build/outputs/apk/debug/app-debug.apk` (x86_64 build)
APK SHA-256: `90063501dad34700369c93aa3a28a5b7f5c038eb33ab401d81afa2fed7139f43`

Sign-off: **PASS — session starts and guest frames flow on SwiftShader.**
Setup needed two fixes, both documented: the AVD ships with no game data
(pushed `game.elf` + the CRC-checked `imported.iso`), and the first x86_64
APK was built without the corpus (`game_corpus_missing`; rebuilt with
`-Pgungnir.gameCorpus`). Screenshot stays in gitignored
`build/qa/2026-11-03-m2102k1ac/`.

## Device

| Field | Value |
|---|---|
| Model | sdk_gphone64_x86_64 (emulator-5554, AVD `gungnir`) |
| Android | 17 |
| SoC platform | SwiftShader (host GL) |
| GPU | SwiftShader (emulator GPU) |
| ABI built | `x86_64` |
| Serial | `emulator-5554` |

## Steps

1. Boot the `gungnir` AVD; push `game.elf` and `imported.iso`.
2. Install the x86_64 + corpus APK; launch.
3. Observe logcat for `game_data_ok`, `session_start`, `first frame`,
   `guest_frame`.

## Expected and actual

| Step | Expected | Actual |
|---|---|---|
| Game data | `game_data_ok`, identity CRC matches | Pass: `size=262766592 crc=183F3EDC` |
| Session | Starts, first frame, frames flow | Pass: `session_start` → `first frame 640x320` in 11ms; `guest_frame=60 draws=31` |
| Shot | Proof screenshot | Pass |

## Screenshot SHA-256

- Emulator: `f6bb8b5d561c570693b42a5d7e068f5f16bc1e1e8914707dc5e8390fb86fa759`

## Notes

- The arm64 APK hash in the other 2026-11-03 reports is unaffected; each
  report records the hash of the exact APK it ran.
- No game frames, dump hashes or generated content stored with this record.

# AUDIO — SAS, ATRAC/MP3 and output

`runtime/audio/` owns the software mixers and the host seam. The PSP title
produces audio through `sceAudio` channel buffers; `sceSasCore` renders VAG
voices into guest memory and `sceAtrac3plus` decodes compressed tracks.

Status: T12a is in progress. The SAS voice core, the sceAudio channel HLE,
the 44.1 kHz mixer, the SDL3 desktop backend, the AAudio backend and the
ATRAC3/3+ decoder and the whole-file ATRAC track HLE are in the tree. The
60 s AAudio soak passes on the phone; ATRAC streaming, MP3 and game-loop
pacing are not finished.

## Output contract

`sceAudio` submits one interleaved s16 buffer per channel with a sample count,
volume and virtual start time. The sink seam is `audio/audio_sink.hpp`; a
missing sink stops the run with `no audio sink registered` rather than
dropping PCM.

`MixerSink` (T12a) turns submissions into one stereo 44.1 kHz ring:

- volumes are `0..0x8000` per channel, applied with saturation;
- mono buffers are duplicated with their left/right volumes;
- `pull()` serves exactly the device request; missing input is silence and
  increments `underrun_frames`;
- a full ring rejects the submission (`overruns`), it never drops audio
  silently;
- `reset()` clears the queue and counters.

The device layer asks for a fixed block (typically 256-1024 frames), so the
underrun count is the real-time health signal for TC-12.2. The boot runner
still installs a counting sink on headless runs. `runtime/platform/sdl_audio.*`
opens an SDL3 S16 stereo stream and pulls through the mixer on desktop;
`runtime/platform/android/aaudio_out.*` opens the same 44.1 kHz S16 stereo
stream through AAudio on Android.

## SAS (sceSasCore)

`runtime/audio/sas_core.*` is adapted from the MIT Yakumo core at the pinned
commit. It implements 32 voices, VAG/ADPCM decode, pitch, ADSR envelopes and
looping at 44.1 kHz. Reverb is not modelled: effect sends are accepted and
ignored. `sceSasCore`/`sceSasCoreWithMix` render into a guest buffer; the
game then submits that buffer through `sceAudio`.

T6g/T6m cover init, voice setup, key on/off, pitch, ADSR, noise and reverb
controls. TC-12.1 host tests cover ADPCM decode, pitch and envelope shapes.

## ATRAC3/3+ and MP3

The FFmpeg n4.4 ATRAC3/3+ subset is vendored from Nakagawa (LGPL-2.1-or-later;
notices and relink path are in `THIRD_PARTY_NOTICES.txt` and the provenance
ledger). `tests/audio/atrac3p_selftest.cpp` checks create/validation, garbage
rejection, the terminator frame, the IMDCT/IPQF path, determinism and
reset/flush.

`runtime/audio/atrac_file.*` parses whole-file RIFF/WAVE ATRAC3 (fmt 0x270)
and ATRAC3+ (fmt 0xFFFE), including `fact` totals/delay and `smpl` loops, and
drives the vendored decoder. The HLE implements the memory-buffer track set:
GetAtracID, SetData/SetDataAndGetID, SetHalfwayBuffer(AndGetID), Release,
GetSoundSample, GetChannel, GetMaxSample, GetRemainFrame, GetNextSample,
GetNextDecodePosition, GetBitrate, GetLoopStatus, SetLoopNum,
ResetPlayPosition, DecodeData and GetInternalErrorInfo. DecodeData copies
guest PCM one codec frame at a time, respects loop/end positions and returns
`0x80630024` after the last frame; a malformed RIFF or failed decode is a
named error, never fake silence.

Streaming calls remain counted gaps: GetStreamDataInfo, AddStreamData,
SetSecondBuffer/GetSecondBufferInfo, GetBufferInfoForReseting, StartEntry and
EndEntry are not registered, so a title that needs them stops with a named
NID instead of receiving silent PCM. `tests/audio/atrac_track_tests.cpp`
covers the parser and decoder with a synthetic ATRAC3+ RIFF; the HLE path is
covered in `tests/hle/kernel_tests.cpp`.

MP3 (`sceMp3`) is not implemented. It stays a named gap until the title
executes an MP3 call; adding a decoder without a caller would be speculative.

## Device targets

Desktop uses `runtime/platform/sdl_audio.*`: an SDL3 S16 stereo 44.1 kHz
stream whose callback pulls from `MixerSink`; `tests/test_sdl_audio.py` plays
a short tone and fails when the device pulls no frames (`sdl_audio_unavailable`
is a named skip).

Android uses `runtime/platform/android/aaudio_out.*`: an AAudio low-latency
44.1 kHz S16 stereo stream whose callback pulls from `MixerSink`, with
pause/resume, an error callback and xrun reporting. Starting the Activity with
`--ez audio_smoke true` runs a short smoke that submits a tone and a synthetic
VAG (rendered by `SasCore`) and logs `audio_smoke ok=... vag=... pulled=...
underrun=... xruns=...`. The duration is `--ei audio_smoke_ms <ms>` (default
500). A normal launch keeps the SDL lifecycle tone and first frame. A 60 s
run pulled 2,647,610 frames with underrun=0 and xruns=0; game-loop pacing and
game audio are wired during T13. See `docs/qa/2026-10-31-t12a-audio.md`.

## Tests

```sh
python3 -m unittest tests.test_audio -v        # ATRAC decoder + track parser/decoder
python3 -m unittest tests.test_audio_mixer -v  # mixer ring/volumes/underrun
python3 -m unittest tests.test_sdl_audio -v     # SDL3 tone smoke (desktop)
# phone (debug APK): am start ... --ez audio_smoke true
python3 -m unittest tests.test_hle -v          # sceAudio + SAS contracts
```

## Named gaps

- ATRAC streaming (GetStreamDataInfo/AddStreamData/second buffer/reset-buffer
  info/start-end entry) and `sceMp3` are not implemented; those calls stop
  with a named NID rather than fake PCM.
- The 60 s AAudio soak is clean, but it runs on the audio clock rather than
  the 59.94 Hz guest loop; that pacing and game audio are T13 work.
- The VAG fixture is synthetic (constant nibble), so it proves the
  SasCore -> mixer -> AAudio path, not audible game audio; game audio follows
  the HLE wiring in T13.
- SAS reverb is ignored, not emulated.
- KIRK/secure audio is out of scope; no key material is ever shipped.

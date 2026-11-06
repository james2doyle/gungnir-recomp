# tiny-AES-c provenance

- Upstream: https://github.com/kokke/tiny-AES-c
- Commit: `23856752fbd139da0b8ca6e471a13d5bcc99a08d`
- License: Unlicense (`UNLICENSE` in this directory, copied verbatim; also
  `LICENSES/Unlicense.txt`)
- Disposition: vendored source, byte-identical to the upstream commit.
  Cross-checked against the unmodified copies in
  `TeamGDB/Yakumo/profiles/mhp3rd/third_party/tiny_aes/`.
- Discovered via: Yakumo `README.md` at
  `profiles/mhp3rd/third_party/tiny_aes/README.md`, which pins the same
  commit and states the files are unmodified.

## What is used

`aes.c` and `aes.h` only. They provide AES-128-CBC for the on-device
`~PSP` EBOOT preparation (`runtime/platform/android/eboot_prepare.cpp`),
ported from Yakumo's `executable_preparation.cpp` behaviour. The build
compiles `aes.c` as C++ (`set_source_files_properties`), the same as
Yakumo does.

## Local patches

None.

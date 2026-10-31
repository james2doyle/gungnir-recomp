# miniz provenance

- Upstream: https://github.com/richgel999/miniz
- Release: `3.1.2`
- Commit: `77d0dce8627735138c51770d1799a1ef48f2117d`
- Release archive: `https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip`
- Archive SHA-256: `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a`
- License: MIT (`LICENSE` in this directory, copied verbatim)
- Disposition: vendored source, byte-identical to the release archive.

## What is used

`miniz.c` and `miniz.h` only. They provide the ZIP container reader/writer
used by the driver package inspector. We do not modify them, and we never
hand-write ZIP parsing or inflation. Driver archives are untrusted input, so
the inspector validates entry names, counts, sizes, encryption and
compression method before extracting anything.

## Why vendored instead of fetched

The Android build and the host tests need the same two files, and the host
test suite must configure and build offline. Vendoring this small,
self-contained source keeps both builds deterministic; the exact commit and
the verified vendored-tree digest are in `dependencies.json`.

## Local patches

None.

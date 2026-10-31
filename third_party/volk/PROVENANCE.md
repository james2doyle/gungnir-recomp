# volk provenance

- Upstream: https://github.com/zeux/volk
- Pinned commit: `aa76c86ae99aa76f7411c981dc885f68a08513b5` (header version 367)
- Archive: `https://github.com/zeux/volk/archive/aa76c86ae99aa76f7411c981dc885f68a08513b5.tar.gz`
- Archive SHA-256: `562a984469b6e93ace6c4e7fe17588ad5e8cb21239adbeb8e52296ac15e54cfb`
- License: MIT (`LICENSE.md` in this directory, copied verbatim)
- Disposition: vendored source, byte-identical to the commit archive.

## What is used

`volk.h` and `volk.c` only. Volk provides the Vulkan entry-point table used on
Android so the system driver and a libadrenotools-opened custom ICD can share
one dispatch path. `volkInitializeCustom()` is given the `vkGetInstanceProcAddr`
returned by the selected loader; `volkLoadInstance()` and `volkLoadDevice()`
populate the remaining entry points. Desktop builds keep the direct Vulkan
link and do not compile Volk.

## Why vendored instead of fetched

The Android runtime and its host-side contract tests need the same two
self-contained files, and the host test suite must configure offline.
Vendoring keeps both builds deterministic; the exact commit and vendored-tree
digest are in `dependencies.json`.

## Local patches

None.

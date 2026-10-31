# TEXTURE REPLACEMENT — dump, keys and user packs

Status: T8c dump mode implemented; T8d replacement loading is pending. This
guide describes the files the runtime writes and the rules a user pack must
follow. No game textures, PNG dumps or pack files are committed.

## Dump layout

Dump with a user-chosen root directory:

```text
<root>/
  textures.ini
  new/
    0123456789ABCDEF.png
    ...
  replace/            (reserved for user replacements; T8d)
```

Dump mode only writes under `new/` and never writes to `replace/`. A PNG that
already exists under `new/` is compared pixel-for-pixel:

- identical: counted as skipped, manifest entry kept;
- different: `key_collision`, nothing overwritten;
- unreadable/corrupt: `existing_png_invalid`, nothing overwritten.

## Content key

The key is 16 uppercase hexadecimal digits, a 64-bit FNV-1a hash over:

```text
"gungnir-tex-v1"
pixel format, swizzle flag, width, height, buffer width
CLUT format/mask, palette entry count, palette format
texel bytes, CLUT bytes
```

The key deliberately excludes live GE sampler state (clamp/filter), so a user
replacement still matches when the game changes filtering. It is
**content-addressed**, not address-addressed: identical textures share a key.

This is **not** a PPSSPP-compatible hash. PPSSPP packs use a 24-hex
address + CLUT + content key; Gungnir's key is shorter and different. Record
the difference in pack documentation rather than silently claiming PPSSPP
compatibility.

## textures.ini

```ini
[options]
version = 1
hash = gungnir64

[hashes]
# 480x272 clut8
0123456789ABCDEF = new/0123456789ABCDEF.png
```

The manifest is append-only: a key already present is not duplicated. The
comment before each entry records dimensions and format for humans. Dump mode
does not yet emit `[hashranges]`; TPL textures are single-level, so mipmap
suffixes (`_0`, `_1`, ...) are not used.

PPSSPP's `[options]`, `[hashes]`, `[hashranges]`, empty-filename skip and
mipmap-level conventions are the behavioral reference
(`hrydgard/ppsspp` PR #8715, wiki, `GPU/Common/TextureReplacer.cpp`). Only the
subset above is implemented; anything else is a named gap until T8d.

## PNG

Dumps are RGBA8 PNGs written with the system libpng (1.6.x on the development
host; libpng/zlib license, GPL-compatible). libpng is a build dependency, not
vendored source. A host without libpng skips the dump target by name instead
of silently producing a different format.

T8d will read user PNGs through the same module, accepting RGB/RGBA/gray and
palette images normalised to RGBA8. Replacement images may be any resolution;
the renderer samples them according to GE state.

## Tests

`tests/test_tex_dump.py` runs `tex_dump_tests` over hand-authored linear
textures: key stability and sensitivity (CLUT, texel, buffer width, swizzle,
format), live-sampler independence, PNG round-trip, manifest creation and
de-duplication, CLUT-change dumps, collision/corrupt-file rejection, and
invalid input names.

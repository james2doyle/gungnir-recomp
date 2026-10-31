# TEXTURE REPLACEMENT — dump, keys and user packs

Status: T8c dump mode and T8d replacement loading implemented. This guide
describes the files the runtime writes and the rules a user pack must follow.
No game textures, PNG dumps or pack files are committed.

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

## Replacement layout

Put user PNGs under `replace/` using the dump key as the filename:

```text
<root>/replace/0123456789ABCDEF.png
```

The loader also accepts a lowercase key and `.PNG`. The image may be 1x, 2x,
4x or any other resolution. There are no mipmap levels; the GE sampling path
maps the sprite's UVs across the replacement regardless of its size.

The key is the same 16-hex content key as dump mode. A replacement is used
when the live texture's key matches; otherwise the native texture stays
active. A PNG whose key matches no known texture is reported by filename by
`TextureReplacements::scan`, never silently ignored.

## Replacement API

```cpp
tex::TextureReplacements replacements(root);
tex::ReplacementLookup found = replacements.find(key);
if (found.ok) {
    ge::TextureState state = found.image->state_for(live.state);
    ge::Texture texture(state, found.image->rgba);
    // sample/draw with texture
} else {
    // use the native texture; found.error names the reason
}
```

- `find` returns `not_found`, `invalid_key`, `invalid_root`,
  `file_access_failed`, `invalid_replacement` or a PNG error
  (`png_read_failed`, `png_unsupported_layout`, `file_not_found`).
- `state_for` copies the live clamp/filter state and overrides format to
  `Rgba8888`, swizzle off, width/height/buffer width from the replacement.
- Successful loads are cached; call `clear()` (or `set_root()`) after changing
  files to reload. A corrupt PNG fails closed; the caller keeps the native
  texture.
- `scan(known_keys)` returns `present` (PNG files), `loaded`, `unmatched`
  filenames (sorted) and per-file `errors` (sorted). The caller logs these;
  the loader does not print or discard them.

Dump mode never writes under `replace/`; a user replacement is never
overwritten by a later dump.

## Font atlas repack

`tools/textures/repack_font_atlas.py` re-rasterizes a dialog font atlas at
2x/4x from a user-supplied glyph source and emits a new TPL plus an ink-width
table:

```sh
python3 tools/textures/repack_font_atlas.py \
  --input font.tpl --glyphs glyphs.json --scale 2 \
  --cell-width 13 --cell-height 13 \
  --output font@2x.tpl --widths widths.json
```

Input shape: a single-texture, 4bpp PSP-swizzled TPL with a 16-entry
RGBA8888 palette (the dialog-font container). The glyph source is JSON:

```json
{"glyphs": [
  {"cell": 1, "code": 65,
   "rows": ["00111100", "01100110", "..."]}
]}
```

`rows` may be hex strings or integer lists; each row is exactly one cell wide
and there is one row per cell height. `cell` is a linear cell index:
`(cell % columns, cell // columns)`. Codes are optional metadata for the width
table.

The tool always proves identity first: decoding and re-encoding the original
atlas must reproduce the input byte-for-byte. If it does not, the tool exits
with `ERR identity_mismatch` and writes nothing. At scale 1 with no glyphs the
output is byte-identical to the input; with glyphs, only the atlas texels
change. At scale >1 the whole atlas is nearest-neighbour scaled, cells and
palette are preserved, and the header is rebuilt for the new dimensions.

The width table records `atlas` metadata and per-cell `left`, `right`, `width`
(+`code` when supplied). The game's advance is the glyph ink width, so this
table is the layout input for text insertion tooling; it is regenerated from
the scaled atlas, never guessed from the original.

Limitations: nearest-neighbour scaling only (no outline rasterizer), and the
input must be the single-texture 4bpp font shape. No game font, glyph bitmap
or translated string is committed; the glyph source is user-supplied.

## Tests

`tests/test_tex_dump.py` runs `tex_dump_tests` over hand-authored linear
textures: key stability and sensitivity (CLUT, texel, buffer width, swizzle,
format), live-sampler independence, PNG round-trip, manifest creation and
de-duplication, CLUT-change dumps, collision/corrupt-file rejection, and
invalid input names.

`tests/test_tex_replace.py` runs `tex_replace_tests`: 2x and 4x replacements
rendered through the software rasterizer, native fallback, state derivation,
unmatched/corrupt scan reporting, cache/clear reload, dump/replacement
isolation and invalid keys/roots.

`tests/test_font_repack.py` exercises the font tool with synthetic atlases:
byte-identical scale-1 identity, 2x/4x glyph and width scaling, a fixed test
string with no ink collisions, modified glyphs at scale 1, and malformed
input/scale/cell refusals that write nothing.

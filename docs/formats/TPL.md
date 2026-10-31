# TPL — Gungnir texture containers

Status: T8a decoder + T8b in-place writer (`runtime/tex/tpl.hpp`). This
describes the Sting PSP `.tpl` container used by Gungnir. It is **not** the
Nintendo/GameCube TPL format; the two share an extension and nothing else.

No TPL bytes, decoded images, palettes or dump hashes are committed. The
fixtures in `tests/tex/tpl_tests.cpp` are synthetic containers authored for
the test.

## Layout

All integers are little-endian. All offsets are absolute file offsets.

```
0x00  u32  n_textures
0x04  u32  table_offset (8 in every observed file)
0x08  table[n_textures] { u32 texture_header_offset; u32 palette_header_offset; }
      ...
      texture headers (0x14 bytes each)
      palette headers (8 bytes each)
      padding to a 16-byte boundary
      texture data / palette data
```

The table is parsed before anything else; header, palette and data positions
are taken from their offsets, never inferred from a fixed formula.

## Texture header (0x14 bytes)

| Offset | Size | Field |
|---|---|---|
| +0x00 | u16 | height in texels |
| +0x02 | u16 | width in texels |
| +0x04 | u16 | buffer width (`BW`) in texels |
| +0x06 | u16 | pixel format (PSP GE `TPF`) |
| +0x08 | u32 | data offset |
| +0x0C | 8 B | opaque; preserved verbatim for the T8b writer |

`BW` is the storage row width and may exceed `width` (the unused tail is
padding). Data size is `BW * height * bits_per_texel / 8`, **not**
`width * height * ...`. Odd widths are valid.

The 8 opaque bytes are observed as zero in many files and non-zero in a few.
They are not interpreted in T8a; the writer must round-trip them.

## Pixel formats

The `pixel format` field uses the PSP GE texture format numbering:

| Value | Format | Bits/texel |
|---|---|---|
| 0 | RGB565 | 16 |
| 1 | RGBA5551 | 16 |
| 2 | RGBA4444 | 16 |
| 3 | RGBA8888 | 32 |
| 4 | CLUT4 | 4 |
| 5 | CLUT8 | 8 |
| 6 | CLUT16 | 16 |
| 7 | CLUT32 | 32 |
| 8-10 | DXT1/3/5 | unsupported (named gap) |
| 0xFFFF | opaque resource | not a texture |

Gungnir's shipped textures use CLUT4 (`4`) and CLUT8 (`5`). Formats 0-3 and
6-7 are accepted by the decoder through the shared GE texture code but are
not exercised by the current fixtures. DXT and unknown format words fail with
`unsupported_pixel_format`; they are never guessed.

Named gap: one local file (`debugFont.tpl`) uses pixel format `0x0108`, which
is not a GE texture format. It fails closed with `unsupported_pixel_format`
and is not decoded by T8a/T8c.

## Palette header (8 bytes)

| Offset | Size | Field |
|---|---|---|
| +0x00 | u16 | entry count (power of two, 1-256) |
| +0x02 | u16 | palette format |
| +0x04 | u32 | palette data offset |

Palette formats: `0` RGB565, `1` RGBA5551, `2` RGBA4444, `3` RGBA8888.
Entry size is 2 bytes for 0-2 and 4 bytes for 3.

The decoder derives the GE CLUT mask from the entry count: direct indexing
with `mask = entries - 1`, no shift, no offset. A CLUT texture without a
palette fails with `missing_palette`.

## Swizzle

Texture data is PSP-swizzled: 16-byte by 8-row blocks stored one after
another, with `blocks_per_row = (BW * bits / 8) / 16`. Decoding uses the same
rule as `runtime/ge/texture.cpp` (T7d). CLUT4 stores two texels per byte, low
nibble first. The decoder requires whole swizzle blocks (`row bytes % 16 ==
0`, `height % 8 == 0`); a container that violates this fails with
`invalid_swizzle_geometry` instead of producing shifted pixels.

Font atlases are CLUT4 with a 16-entry RGBA8888 palette whose entries are
white with an alpha ramp of `index * 17` (0, 17, ..., 255). There is no
separate alpha format: the palette carries the ramp.

## Auxiliary resources

A table entry with `pixel_format == 0xFFFF` and zero dimensions is an opaque
auxiliary resource (animation/frame tables are observed in sprite containers).
Its data offset is still bounds-checked; its size is bounded by the next known
data/palette offset or the end of the file. These entries are counted in
`Tpl::resources` and never silently dropped. The declared texture count
includes them.

## Decoder API

```cpp
tex::Tpl tpl = tex::load_tpl(bytes);       // or load_tpl_file(path)
if (!tpl.ok) { /* tpl.error is a named code */ }
for (const tex::TplTexture &texture : tpl.textures)
    std::vector<std::uint32_t> rgba = tex::decode_texture(texture);
```

Named errors: `file_not_found`, `truncated_header`, `invalid_texture_count`,
`invalid_table_offset`, `truncated_entry_table`,
`texture_header_out_of_range`, `unsupported_pixel_format`,
`invalid_texture_dimensions`, `invalid_buffer_width`,
`invalid_swizzle_geometry`, `data_out_of_range`,
`resource_out_of_range`, `palette_header_out_of_range`,
`unsupported_palette_format`, `invalid_palette_entries`,
`palette_out_of_range`, `missing_palette`.

`Tpl::bytes` keeps the original container so the T8b writer can prove a
byte-identical round-trip. `TplTexture::header` and `palette_header` keep the
raw descriptor bytes for the same reason.

## Writer

```cpp
tex::TplWriteResult written = tex::write_tpl(tpl);
if (!written.ok) { /* written.error is a named code */ }
```

`write_tpl` is an **in-place** writer: it starts from `Tpl::bytes`, rewrites
the header, table, texture/palette descriptors and the texture/palette bytes
at their recorded offsets, and leaves padding, auxiliary-resource headers and
any unknown tail bytes untouched. An unmodified decode -> write is
byte-identical. Changing `TplTexture::data` or `palette` changes exactly those
byte ranges; changing dimensions requires the matching data size.

Writer errors: `invalid_model`, `invalid_table_offset`,
`invalid_texture_count`, `table_mismatch`, `entry_count_mismatch`,
`truncated_header`, `truncated_entry_table`, `texture_header_out_of_range`,
`unsupported_pixel_format`, `invalid_texture_dimensions`,
`invalid_buffer_width`, `invalid_swizzle_geometry`, `data_size_mismatch`,
`data_out_of_range`, `palette_header_out_of_range`,
`invalid_palette_entries`, `unsupported_palette_format`,
`palette_size_mismatch`, `palette_out_of_range`, `missing_palette`,
`resource_header_out_of_range`, `resource_out_of_range`.

Resources are opaque: the writer preserves their raw descriptor bytes and data
verbatim.

## Local validation

On 2026-10-31 the decoder was run over the user's local `INI.DAR`/`DATA.DAR`
(extracted to a temporary directory, not committed) with ASan/UBSan enabled:
1,304 of 1,305 unique containers decoded, covering 1,886 textures / 58,952,576
pixels and 564 auxiliary resources. The same 1,304 containers round-tripped
through `write_tpl` byte-identically. The only failure is `debugFont.tpl`
(pixel format `0x0108`), which fails closed as recorded above.

## References

- `kreycai/gungnir-ptbr` `tools/fonte_gungnir.py` and
  `docs/graficos_com_texto.md`: header fields, 4bpp swizzle, palette/alpha
  ramp facts, and the single-texture decode/encode proof.
- Local header inspection of the user's `INI.DAR`/`DATA.DAR` (2026-10-31):
  table layout, palette descriptors, multi-texture containers and the
  `0xFFFF` resource sentinel. No bytes copied.
- `marco-calautti/Rainbow` `TPLTexture.Serializer.cs`: multi-image table
  structure and named-error style for the separate Nintendo TPL format; the
  PSP variant above is not that format.
- `LiinkPK/Texture-Replacement-Tool-for-PPSSPP`: replacement workflow and
  `textures.ini` UX reference for T8c/T8d; binary-only, no source adopted.

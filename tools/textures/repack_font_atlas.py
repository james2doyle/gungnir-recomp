#!/usr/bin/env python3
"""Repack a dialog font atlas at Nx from a user-supplied glyph source.

The input is a single-texture TPL with 4bpp PSP-swizzled texels and a
16-entry RGBA8888 palette (the dialog-font shape). Glyph bitmaps are supplied
at the base cell size as JSON; they are overlaid on the decoded atlas, the
whole atlas is scaled by N with nearest-neighbour replication, and the result
is written as a new TPL plus an ink-width table.

Before writing anything the tool proves the identity invariant: decoding and
re-encoding the original atlas must reproduce the input byte-for-byte. If that
check fails, no output is written.

No game font, glyph bitmap or translated string is committed or shipped.
"""
import argparse
import json
import struct
import sys
from pathlib import Path


class TplError(Exception):
    pass


def _u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def _put_u16(data, offset, value):
    struct.pack_into("<H", data, offset, value)


def _put_u32(data, offset, value):
    struct.pack_into("<I", data, offset, value)


def _unswizzle(body, row_bytes, height):
    if row_bytes % 16 or height % 8:
        raise TplError("unsupported_swizzle_geometry")
    blocks_per_row = row_bytes // 16
    linear = bytearray(row_bytes * height)
    source = 0
    for band in range(height // 8):
        for block in range(blocks_per_row):
            for row in range(8):
                target = (band * 8 + row) * row_bytes + block * 16
                linear[target:target + 16] = body[source:source + 16]
                source += 16
    return linear


def _swizzle(linear, row_bytes, height):
    if row_bytes % 16 or height % 8:
        raise TplError("unsupported_swizzle_geometry")
    blocks_per_row = row_bytes // 16
    out = bytearray(row_bytes * height)
    target = 0
    for band in range(height // 8):
        for block in range(blocks_per_row):
            for row in range(8):
                source = (band * 8 + row) * row_bytes + block * 16
                out[target:target + 16] = linear[source:source + 16]
                target += 16
    return out


class Atlas:
    def __init__(self, blob, height, width, buffer_width, data_offset, palette_offset,
                 palette_entries, palette_format, palette_data_offset, head, tail, pixels):
        self.blob = blob
        self.height = height
        self.width = width
        self.buffer_width = buffer_width
        self.data_offset = data_offset
        self.palette_offset = palette_offset
        self.palette_entries = palette_entries
        self.palette_format = palette_format
        self.palette_data_offset = palette_data_offset
        self.head = head
        self.tail = tail
        self.pixels = pixels
        self.cell_width = 0
        self.cell_height = 0


def decode_tpl(blob):
    if len(blob) < 0x30:
        raise TplError("invalid_tpl")
    if _u32(blob, 0) != 1 or _u32(blob, 4) != 8:
        raise TplError("unsupported_tpl")
    header_offset = _u32(blob, 8)
    palette_offset = _u32(blob, 12)
    if header_offset + 0x14 > len(blob):
        raise TplError("invalid_tpl")
    height, width, buffer_width, bpp = struct.unpack_from("<4H", blob, header_offset)
    data_offset = _u32(blob, header_offset + 8)
    if bpp != 4:
        raise TplError("unsupported_pixel_format")
    if height == 0 or width == 0 or buffer_width < width:
        raise TplError("invalid_dimensions")
    if buffer_width % 2 or width % 2:
        raise TplError("invalid_dimensions")
    if palette_offset == 0 or palette_offset + 8 > len(blob):
        raise TplError("missing_palette")
    entries, palette_format = struct.unpack_from("<2H", blob, palette_offset)
    palette_data_offset = _u32(blob, palette_offset + 4)
    if entries != 16 or palette_format != 3:
        raise TplError("unsupported_palette")
    row_bytes = buffer_width // 2
    data_size = row_bytes * height
    if data_offset + data_size > len(blob):
        raise TplError("invalid_tpl")
    if palette_data_offset + entries * 4 > len(blob):
        raise TplError("invalid_tpl")
    head = blob[:data_offset]
    tail = blob[data_offset + data_size:]
    linear = _unswizzle(blob[data_offset:data_offset + data_size], row_bytes, height)
    pixels = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            byte = linear[y * row_bytes + x // 2]
            pixels[y * width + x] = (byte & 0x0F) if x % 2 == 0 else (byte >> 4)
    return Atlas(blob, height, width, buffer_width, data_offset, palette_offset,
                 entries, palette_format, palette_data_offset, head, tail, pixels)


def encode_like_original(atlas, pixels):
    row_bytes = atlas.buffer_width // 2
    linear = bytearray(row_bytes * atlas.height)
    for y in range(atlas.height):
        for x in range(atlas.width):
            value = pixels[y * atlas.width + x] & 0x0F
            if x % 2 == 0:
                linear[y * row_bytes + x // 2] |= value
            else:
                linear[y * row_bytes + x // 2] |= value << 4
    return atlas.head + _swizzle(linear, row_bytes, atlas.height) + atlas.tail


def _palette_bytes(atlas):
    start = atlas.palette_data_offset
    return atlas.blob[start:start + atlas.palette_entries * 4]


def _extra_tail(atlas):
    start = atlas.palette_data_offset + atlas.palette_entries * 4
    return atlas.blob[start:]


def build_scaled(atlas, pixels, scale):
    if scale < 1:
        raise TplError("invalid_scale")
    height = atlas.height * scale
    width = atlas.width * scale
    buffer_width = atlas.buffer_width * scale
    row_bytes = buffer_width // 2
    if row_bytes % 16 or height % 8:
        raise TplError("unsupported_swizzle_geometry")
    data_size = row_bytes * height
    palette_offset = 0x24
    palette_data_offset = 0x30 + data_size
    head = bytearray(0x30)
    original_head = atlas.head[:0x30]
    head[:len(original_head)] = original_head
    _put_u32(head, 0, 1)
    _put_u32(head, 4, 8)
    _put_u32(head, 8, 0x10)
    _put_u32(head, 12, palette_offset)
    _put_u16(head, 0x10, height)
    _put_u16(head, 0x12, width)
    _put_u16(head, 0x14, buffer_width)
    _put_u16(head, 0x16, 4)
    _put_u32(head, 0x18, 0x30)
    _put_u16(head, palette_offset, atlas.palette_entries)
    _put_u16(head, palette_offset + 2, atlas.palette_format)
    _put_u32(head, palette_offset + 4, palette_data_offset)
    linear = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            value = pixels[(y // scale) * atlas.width + (x // scale)] & 0x0F
            if x % 2 == 0:
                linear[y * row_bytes + x // 2] |= value
            else:
                linear[y * row_bytes + x // 2] |= value << 4
    return bytes(head) + _swizzle(linear, row_bytes, height) + _palette_bytes(atlas) + _extra_tail(atlas)


def scale_pixels(pixels, width, height, scale):
    out = bytearray(width * scale * height * scale)
    for y in range(height * scale):
        for x in range(width * scale):
            out[y * width * scale + x] = pixels[(y // scale) * width + (x // scale)]
    return out


def _glyph_rows(entry, cell_width, cell_height):
    rows = entry.get("rows")
    if not isinstance(rows, list) or len(rows) != cell_height:
        raise TplError("invalid_glyph")
    parsed = []
    for row in rows:
        if isinstance(row, str):
            text = "".join(row.split())
            if len(text) != cell_width:
                raise TplError("invalid_glyph")
            try:
                values = [int(c, 16) for c in text]
            except ValueError as exc:
                raise TplError("invalid_glyph") from exc
        elif isinstance(row, list) and len(row) == cell_width:
            try:
                values = [int(v) for v in row]
            except (TypeError, ValueError) as exc:
                raise TplError("invalid_glyph") from exc
        else:
            raise TplError("invalid_glyph")
        if any(value < 0 or value > 15 for value in values):
            raise TplError("invalid_glyph")
        parsed.append(values)
    return parsed


def load_glyphs(path):
    if path is None:
        return [], {}
    try:
        data = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise TplError("invalid_glyph_source") from exc
    entries = data.get("glyphs") if isinstance(data, dict) else data
    if not isinstance(entries, list):
        raise TplError("invalid_glyph_source")
    glyphs = []
    codes = {}
    for entry in entries:
        if not isinstance(entry, dict) or "cell" not in entry:
            raise TplError("invalid_glyph_source")
        try:
            cell = int(entry["cell"])
        except (TypeError, ValueError) as exc:
            raise TplError("invalid_glyph_source") from exc
        glyphs.append((cell, entry))
        if "code" in entry:
            try:
                codes[cell] = int(entry["code"])
            except (TypeError, ValueError) as exc:
                raise TplError("invalid_glyph_source") from exc
    return glyphs, codes


def overlay_glyphs(atlas, pixels, glyphs):
    columns = atlas.width // atlas.cell_width
    rows = atlas.height // atlas.cell_height
    if columns <= 0 or rows <= 0:
        raise TplError("invalid_dimensions")
    for cell, entry in glyphs:
        if cell < 0 or cell >= columns * rows:
            raise TplError("glyph_out_of_range")
        bitmap = _glyph_rows(entry, atlas.cell_width, atlas.cell_height)
        column, row = cell % columns, cell // columns
        for y in range(atlas.cell_height):
            for x in range(atlas.cell_width):
                pixels[(row * atlas.cell_height + y) * atlas.width + column * atlas.cell_width + x] = bitmap[y][x]


def set_cell_shape(atlas, cell_width, cell_height):
    if cell_width <= 0 or cell_height <= 0 or atlas.width % cell_width:
        raise TplError("invalid_cell_geometry")
    if atlas.width // cell_width <= 0 or atlas.height // cell_height <= 0:
        raise TplError("invalid_cell_geometry")
    atlas.cell_width = cell_width
    atlas.cell_height = cell_height


def measure_widths(pixels, width, height, cell_width, cell_height, codes):
    columns = width // cell_width
    rows = height // cell_height
    if columns <= 0 or rows <= 0:
        raise TplError("invalid_dimensions")
    out = []
    for cell in range(columns * rows):
        column, row = cell % columns, cell // columns
        left = cell_width
        right = -1
        for y in range(cell_height):
            for x in range(cell_width):
                if pixels[(row * cell_height + y) * width + column * cell_width + x]:
                    left = min(left, x)
                    right = max(right, x)
        entry = {"cell": cell, "left": left if right >= 0 else 0,
                 "right": right if right >= 0 else -1,
                 "width": right - left + 1 if right >= 0 else 0}
        if cell in codes:
            entry["code"] = codes[cell]
        out.append(entry)
    return out


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, help="single-texture 4bpp font TPL")
    parser.add_argument("--glyphs", help="glyph bitmap JSON")
    parser.add_argument("--scale", type=int, default=1, help="integer upscale (1-8)")
    parser.add_argument("--output", required=True, help="output TPL")
    parser.add_argument("--widths", help="output ink-width JSON")
    parser.add_argument("--cell-width", type=int, required=True)
    parser.add_argument("--cell-height", type=int, required=True)
    args = parser.parse_args(argv)

    try:
        if args.scale < 1 or args.scale > 8:
            raise TplError("invalid_scale")
        try:
            blob = Path(args.input).read_bytes()
        except OSError as exc:
            raise TplError("input_not_found") from exc
        atlas = decode_tpl(blob)
        if encode_like_original(atlas, atlas.pixels) != blob:
            raise TplError("identity_mismatch")
        set_cell_shape(atlas, args.cell_width, args.cell_height)
        glyphs, codes = load_glyphs(args.glyphs)
        pixels = bytearray(atlas.pixels)
        overlay_glyphs(atlas, pixels, glyphs)
        if args.scale == 1:
            output = encode_like_original(atlas, pixels)
            measured_pixels = pixels
        else:
            measured_pixels = scale_pixels(pixels, atlas.width, atlas.height, args.scale)
            output = build_scaled(atlas, pixels, args.scale)
        measured_width = atlas.width * args.scale
        measured_height = atlas.height * args.scale
        widths = {
            "atlas": {
                "width": measured_width,
                "height": measured_height,
                "cell_width": atlas.cell_width * args.scale,
                "cell_height": atlas.cell_height * args.scale,
                "columns": atlas.width // atlas.cell_width,
                "scale": args.scale,
            },
            "glyphs": measure_widths(measured_pixels, measured_width, measured_height,
                                      atlas.cell_width * args.scale,
                                      atlas.cell_height * args.scale, codes),
        }
        try:
            if args.widths:
                Path(args.widths).write_text(json.dumps(widths, indent=2) + "\n")
            Path(args.output).write_bytes(output)
        except OSError as exc:
            raise TplError("output_write_failed") from exc
        return 0
    except TplError as error:
        print(f"ERR {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())

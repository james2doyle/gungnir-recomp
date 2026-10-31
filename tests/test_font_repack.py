#!/usr/bin/env python3
"""T8e — font atlas repack, ink-width regeneration and identity gate.

All atlases and glyph bitmaps are synthetic: no game font bytes are used or
committed.
"""
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "textures" / "repack_font_atlas.py"

CELL_W = 8
CELL_H = 8
COLUMNS = 4
WIDTH = 32
HEIGHT = 32

GLYPHS = [
    {
        "cell": 1,
        "code": 65,
        "rows": [
            "00111100",
            "01100110",
            "01100110",
            "01111110",
            "01100110",
            "01100110",
            "01100110",
            "00000000",
        ],
    },
    {
        "cell": 2,
        "code": 105,
        "rows": [
            "00011000",
            "00011000",
            "00011000",
            "00011000",
            "00011000",
            "00011000",
            "00011000",
            "00000000",
        ],
    },
    {
        "cell": 5,
        "code": 87,
        "rows": [
            "11000011",
            "11000011",
            "11000011",
            "11011011",
            "11011011",
            "11111111",
            "11100111",
            "00000000",
        ],
    },
]


def _swizzle(pixels, width, height):
    row_bytes = width // 2
    linear = bytearray(row_bytes * height)
    for y in range(height):
        for x in range(width):
            value = pixels[y * width + x] & 0x0F
            if x % 2 == 0:
                linear[y * row_bytes + x // 2] |= value
            else:
                linear[y * row_bytes + x // 2] |= value << 4
    out = bytearray(row_bytes * height)
    target = 0
    for band in range(height // 8):
        for block in range(row_bytes // 16):
            for row in range(8):
                source = (band * 8 + row) * row_bytes + block * 16
                out[target:target + 16] = linear[source:source + 16]
                target += 16
    return out


def _unswizzle(body, width, height):
    row_bytes = width // 2
    linear = bytearray(row_bytes * height)
    source = 0
    for band in range(height // 8):
        for block in range(row_bytes // 16):
            for row in range(8):
                target = (band * 8 + row) * row_bytes + block * 16
                linear[target:target + 16] = body[source:source + 16]
                source += 16
    pixels = bytearray(width * height)
    for y in range(height):
        for x in range(width):
            byte = linear[y * row_bytes + x // 2]
            pixels[y * width + x] = (byte & 0x0F) if x % 2 == 0 else (byte >> 4)
    return pixels


def make_tpl(pixels):
    data = _swizzle(pixels, WIDTH, HEIGHT)
    palette = bytearray()
    for i in range(16):
        palette += bytes([255, 255, 255, i * 17])
    header = bytearray(0x30)
    struct.pack_into("<4I", header, 0, 1, 8, 0x10, 0x24)
    struct.pack_into("<4H", header, 0x10, HEIGHT, WIDTH, WIDTH, 4)
    struct.pack_into("<I", header, 0x18, 0x30)
    struct.pack_into("<I", header, 0x1C, 0x00010001)
    struct.pack_into("<I", header, 0x20, 0)
    struct.pack_into("<HHI", header, 0x24, 16, 3, 0x30 + len(data))
    return bytes(header) + bytes(data) + bytes(palette)


def decode_tpl(blob):
    if struct.unpack_from("<I", blob, 0)[0] != 1 or struct.unpack_from("<I", blob, 4)[0] != 8:
        raise AssertionError("unexpected TPL header")
    header = struct.unpack_from("<I", blob, 8)[0]
    palette_header = struct.unpack_from("<I", blob, 12)[0]
    height, width, buffer_width, bpp = struct.unpack_from("<4H", blob, header)
    data_offset = struct.unpack_from("<I", blob, header + 8)[0]
    entries, palette_format, palette_offset = struct.unpack_from("<HHI", blob, palette_header)
    if bpp != 4 or entries != 16 or palette_format != 3:
        raise AssertionError("unexpected TPL shape")
    data_size = (buffer_width // 2) * height
    pixels = _unswizzle(blob[data_offset:data_offset + data_size], width, height)
    return {
        "width": width,
        "height": height,
        "buffer_width": buffer_width,
        "pixels": pixels,
        "palette": blob[palette_offset:palette_offset + entries * 4],
    }


def overlay_glyphs(pixels, glyphs):
    out = bytearray(pixels)
    for glyph in glyphs:
        cell = glyph["cell"]
        column, row = cell % COLUMNS, cell // COLUMNS
        for y, line in enumerate(glyph["rows"]):
            for x, char in enumerate(line):
                out[(row * CELL_H + y) * WIDTH + column * CELL_W + x] = int(char, 16)
    return out


def upscale(pixels, scale):
    out = bytearray(WIDTH * scale * HEIGHT * scale)
    for y in range(HEIGHT * scale):
        for x in range(WIDTH * scale):
            out[y * WIDTH * scale + x] = pixels[(y // scale) * WIDTH + (x // scale)]
    return out


def run_tool(tmp, source, scale=1, glyphs=None, name="out"):
    output = tmp / f"{name}.tpl"
    widths = tmp / f"{name}.json"
    command = [
        sys.executable,
        str(TOOL),
        "--input", str(source),
        "--scale", str(scale),
        "--output", str(output),
        "--widths", str(widths),
        "--cell-width", str(CELL_W),
        "--cell-height", str(CELL_H),
    ]
    if glyphs is not None:
        glyph_file = tmp / "glyphs.json"
        glyph_file.write_text(json.dumps({"glyphs": glyphs}))
        command += ["--glyphs", str(glyph_file)]
    proc = subprocess.run(command, capture_output=True, text=True)
    return proc, output, widths


class TestFontRepack(unittest.TestCase):
    def test_identity_gate(self):
        pixels = bytearray(WIDTH * HEIGHT)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = tmp / "base.tpl"
            source.write_bytes(make_tpl(pixels))
            proc, output, widths = run_tool(tmp, source)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(output.read_bytes(), source.read_bytes(), "scale 1 is byte-identical")
            table = json.loads(widths.read_text())
            self.assertEqual(table["atlas"]["width"], WIDTH)
            self.assertEqual(table["atlas"]["cell_width"], CELL_W)

    def test_scale_two_and_four(self):
        base_pixels = overlay_glyphs(bytearray(WIDTH * HEIGHT), GLYPHS)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = tmp / "base.tpl"
            source.write_bytes(make_tpl(bytearray(WIDTH * HEIGHT)))
            for scale in (2, 4):
                proc, output, widths = run_tool(tmp, source, scale=scale, glyphs=GLYPHS,
                                                name=f"out{scale}")
                self.assertEqual(proc.returncode, 0, proc.stderr)
                decoded = decode_tpl(output.read_bytes())
                self.assertEqual((decoded["width"], decoded["height"]),
                                 (WIDTH * scale, HEIGHT * scale))
                self.assertEqual(decoded["pixels"], upscale(base_pixels, scale),
                                 "glyphs scale with the atlas")
                table = json.loads(widths.read_text())
                by_cell = {entry["cell"]: entry for entry in table["glyphs"]}
                for glyph, expected_left, expected_width in [(1, 1, 6), (2, 3, 2), (5, 0, 8)]:
                    entry = by_cell[glyph]
                    self.assertEqual(entry["left"], expected_left * scale)
                    self.assertEqual(entry["right"], expected_left * scale + expected_width * scale - 1)
                    self.assertEqual(entry["width"], expected_width * scale)
                self.assertEqual(by_cell[1]["code"], 65)
                self.assertEqual(by_cell[2]["code"], 105)
                self.assertEqual(by_cell[5]["code"], 87)

    def test_fixed_string_has_no_collisions(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = tmp / "base.tpl"
            source.write_bytes(make_tpl(bytearray(WIDTH * HEIGHT)))
            proc, output, widths = run_tool(tmp, source, scale=2, glyphs=GLYPHS)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            table = json.loads(widths.read_text())
            by_cell = {entry["cell"]: entry for entry in table["glyphs"]}
            decoded = decode_tpl(output.read_bytes())
            cells = [1, 2, 5]
            total_width = sum(by_cell[cell]["width"] for cell in cells)
            canvas_width = total_width
            canvas = [0] * (canvas_width * CELL_H * 2)
            cursor = 0
            for cell in cells:
                column, row = cell % COLUMNS, cell // COLUMNS
                entry = by_cell[cell]
                for y in range(CELL_H * 2):
                    for x in range(CELL_W * 2):
                        value = decoded["pixels"][(row * CELL_H * 2 + y) * WIDTH * 2 +
                                                  column * CELL_W * 2 + x]
                        if not value:
                            continue
                        target_x = cursor + x - entry["left"]
                        self.assertGreaterEqual(target_x, 0, "glyph stays inside its advance")
                        self.assertLess(target_x, canvas_width, "glyph stays inside the string")
                        index = y * canvas_width + target_x
                        self.assertEqual(canvas[index], 0, "glyph ink does not collide")
                        canvas[index] = value
                cursor += entry["width"]
            self.assertEqual(cursor, canvas_width)

    def test_modified_glyph_at_scale_one(self):
        pixels = bytearray(WIDTH * HEIGHT)
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = tmp / "base.tpl"
            source.write_bytes(make_tpl(pixels))
            proc, output, _ = run_tool(tmp, source, glyphs=GLYPHS)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertNotEqual(output.read_bytes(), source.read_bytes())
            decoded = decode_tpl(output.read_bytes())
            self.assertEqual(decoded["pixels"], overlay_glyphs(pixels, GLYPHS))

    def test_refuses_bad_inputs_without_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            truncated = tmp / "truncated.tpl"
            truncated.write_bytes(make_tpl(bytearray(WIDTH * HEIGHT))[:0x20])
            proc, output, _ = run_tool(tmp, truncated, name="bad")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("ERR", proc.stderr)
            self.assertFalse(output.exists(), "invalid input writes nothing")

            source = tmp / "base.tpl"
            source.write_bytes(make_tpl(bytearray(WIDTH * HEIGHT)))
            proc, output, _ = run_tool(tmp, source, scale=0, name="badscale")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("invalid_scale", proc.stderr)
            self.assertFalse(output.exists(), "invalid scale writes nothing")

            out_of_range = [{"cell": 999, "rows": ["00000000"] * CELL_H}]
            proc, output, _ = run_tool(tmp, source, glyphs=out_of_range, name="badcell")
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("glyph_out_of_range", proc.stderr)
            self.assertFalse(output.exists(), "out-of-range glyph writes nothing")


if __name__ == "__main__":
    unittest.main()

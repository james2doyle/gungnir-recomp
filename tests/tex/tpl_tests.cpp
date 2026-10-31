// T8a contract tests: TPL container decoding.
//
// Every container below is hand-authored for this fixture: no game TPL bytes
// are committed or read by the test.

#include "tex/tpl.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir;
using namespace gungnir::tex;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void write_u16(std::vector<std::uint8_t> &data, std::size_t offset, std::uint16_t value) {
    data[offset] = static_cast<std::uint8_t>(value & 0xFFu);
    data[offset + 1] = static_cast<std::uint8_t>(value >> 8u);
}

void write_u32(std::vector<std::uint8_t> &data, std::size_t offset, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        data[offset + static_cast<std::size_t>(shift / 8)] =
            static_cast<std::uint8_t>(value >> shift);
}

void append_u16(std::vector<std::uint8_t> &data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    data.push_back(static_cast<std::uint8_t>(value >> 8u));
}

void append_u32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        data.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

struct TextureSpec {
    int width{};
    int height{};
    int buffer_width{};
    std::uint16_t format{};
    std::vector<std::uint8_t> indices;
    std::vector<std::uint32_t> palette;
    std::uint16_t palette_format{3};
    bool resource{};
    std::vector<std::uint8_t> resource_bytes;
};

std::vector<std::uint8_t> swizzle_indices(const TextureSpec &spec) {
    const int bits = spec.format == 4 ? 4 : spec.format == 5 ? 8 : 0;
    check(bits != 0, "fixture format must be CLUT4 or CLUT8");
    const int row_bytes = spec.buffer_width * bits / 8;
    check(row_bytes % 16 == 0 && spec.height % 8 == 0, "fixture uses whole swizzle blocks");
    std::vector<std::uint8_t> linear(static_cast<std::size_t>(row_bytes) *
                                         static_cast<std::size_t>(spec.height),
                                     0);
    for (int y = 0; y < spec.height; ++y) {
        for (int x = 0; x < spec.width; ++x) {
            const std::uint8_t raw_index =
                spec.indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(spec.width) +
                             static_cast<std::size_t>(x)];
            const std::uint8_t index = bits == 4 ? static_cast<std::uint8_t>(raw_index & 15u) : raw_index;
            const std::size_t offset = static_cast<std::size_t>(y) *
                                           static_cast<std::size_t>(row_bytes) +
                                       static_cast<std::size_t>(x) * static_cast<std::size_t>(bits) / 8u;
            if (bits == 4) {
                if (x % 2 == 0)
                    linear[offset] = static_cast<std::uint8_t>(linear[offset] | index);
                else
                    linear[offset] = static_cast<std::uint8_t>(linear[offset] | (index << 4u));
            } else {
                linear[offset] = index;
            }
        }
    }
    std::vector<std::uint8_t> out(linear.size());
    const int blocks_per_row = row_bytes / 16;
    std::size_t dst = 0;
    for (int by = 0; by < spec.height / 8; ++by)
        for (int bx = 0; bx < blocks_per_row; ++bx)
            for (int row = 0; row < 8; ++row) {
                std::memcpy(out.data() + dst,
                            linear.data() +
                                static_cast<std::size_t>(by * 8 + row) *
                                    static_cast<std::size_t>(row_bytes) +
                                static_cast<std::size_t>(bx) * 16u,
                            16);
                dst += 16;
            }
    return out;
}

void append_palette(std::vector<std::uint8_t> &out, const TextureSpec &spec) {
    for (const std::uint32_t color : spec.palette) {
        const std::uint32_t red = color & 255u;
        const std::uint32_t green = (color >> 8u) & 255u;
        const std::uint32_t blue = (color >> 16u) & 255u;
        const std::uint32_t alpha = (color >> 24u) & 255u;
        if (spec.palette_format == 3) {
            append_u32(out, color);
        } else if (spec.palette_format == 0) {
            append_u16(out, static_cast<std::uint16_t>((red >> 3u) | ((green >> 2u) << 5u) |
                                                       ((blue >> 3u) << 11u)));
        } else if (spec.palette_format == 1) {
            append_u16(out, static_cast<std::uint16_t>((red >> 3u) | ((green >> 3u) << 5u) |
                                                       ((blue >> 3u) << 10u) |
                                                       ((alpha >> 7u) << 15u)));
        } else if (spec.palette_format == 2) {
            append_u16(out, static_cast<std::uint16_t>((red >> 4u) | ((green >> 4u) << 4u) |
                                                       ((blue >> 4u) << 8u) |
                                                       ((alpha >> 4u) << 12u)));
        } else {
            throw std::runtime_error("fixture palette format is unsupported");
        }
    }
}

std::vector<std::uint8_t> build_tpl(const std::vector<TextureSpec> &specs) {
    const std::size_t count = specs.size();
    const std::size_t tex_base = 8 + count * 8;
    const std::size_t pal_base = tex_base + count * 0x14;
    std::size_t cursor = align_up(pal_base + count * 8, 16);
    std::vector<std::uint8_t> out(cursor, 0);
    write_u32(out, 0, static_cast<std::uint32_t>(count));
    write_u32(out, 4, 8);

    for (std::size_t i = 0; i < count; ++i) {
        const TextureSpec &spec = specs[i];
        const std::size_t tex_header = tex_base + i * 0x14;
        const std::size_t pal_header = pal_base + i * 8;
        write_u32(out, 8 + i * 8, static_cast<std::uint32_t>(tex_header));
        cursor = align_up(cursor, 16);
        const std::uint32_t data_offset = static_cast<std::uint32_t>(cursor);
        std::vector<std::uint8_t> data =
            spec.resource ? spec.resource_bytes : swizzle_indices(spec);
        out.resize(cursor + data.size());
        std::memcpy(out.data() + cursor, data.data(), data.size());
        cursor += data.size();

        write_u16(out, tex_header + 0, static_cast<std::uint16_t>(spec.resource ? 0 : spec.height));
        write_u16(out, tex_header + 2, static_cast<std::uint16_t>(spec.resource ? 0 : spec.width));
        write_u16(out, tex_header + 4,
                  static_cast<std::uint16_t>(spec.resource ? 0 : spec.buffer_width));
        write_u16(out, tex_header + 6,
                  static_cast<std::uint16_t>(spec.resource ? 0xFFFFu : spec.format));
        write_u32(out, tex_header + 8, data_offset);

        if (!spec.resource && !spec.palette.empty()) {
            cursor = align_up(cursor, 16);
            const std::uint32_t palette_offset = static_cast<std::uint32_t>(cursor);
            write_u32(out, 8 + i * 8 + 4, static_cast<std::uint32_t>(pal_header));
            write_u16(out, pal_header + 0, static_cast<std::uint16_t>(spec.palette.size()));
            write_u16(out, pal_header + 2, spec.palette_format);
            write_u32(out, pal_header + 4, palette_offset);
            std::vector<std::uint8_t> palette_bytes;
            append_palette(palette_bytes, spec);
            out.resize(cursor + palette_bytes.size());
            std::memcpy(out.data() + cursor, palette_bytes.data(), palette_bytes.size());
            cursor += palette_bytes.size();
        }
    }
    out.resize(cursor);
    return out;
}

TextureSpec clut4_spec() {
    TextureSpec spec;
    spec.width = 6;
    spec.height = 8;
    spec.buffer_width = 32;
    spec.format = 4;
    spec.indices.resize(static_cast<std::size_t>(spec.width * spec.height));
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x)
            spec.indices[static_cast<std::size_t>(y * spec.width + x)] =
                static_cast<std::uint8_t>((x + y * spec.width) % 16);
    spec.palette.resize(16);
    for (int i = 0; i < 16; ++i)
        spec.palette[static_cast<std::size_t>(i)] =
            ge::rgba8(255u, 255u, 255u, static_cast<std::uint32_t>(i) * 17u);
    return spec;
}

TextureSpec clut4_palette565_spec() {
    TextureSpec spec = clut4_spec();
    spec.palette_format = 0;
    spec.palette.assign(16, ge::rgba8(0u, 0u, 0u));
    spec.palette[0] = ge::rgba8(255u, 0u, 0u);
    spec.palette[1] = ge::rgba8(0u, 255u, 0u);
    spec.palette[2] = ge::rgba8(0u, 0u, 255u);
    spec.palette[3] = ge::rgba8(255u, 255u, 255u);
    return spec;
}

TextureSpec clut8_spec() {
    TextureSpec spec;
    spec.width = 4;
    spec.height = 8;
    spec.buffer_width = 16;
    spec.format = 5;
    spec.indices.resize(static_cast<std::size_t>(spec.width * spec.height));
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x)
            spec.indices[static_cast<std::size_t>(y * spec.width + x)] =
                static_cast<std::uint8_t>((x * 7 + y * 13) & 0xFF);
    spec.palette.resize(256);
    for (int i = 0; i < 256; ++i)
        spec.palette[static_cast<std::size_t>(i)] =
            ge::rgba8(static_cast<std::uint32_t>(i),
                      static_cast<std::uint32_t>(255 - i),
                      static_cast<std::uint32_t>((i * 3) & 0xFF), 255u);
    return spec;
}

std::vector<std::uint32_t> expected_pixels(const TextureSpec &spec) {
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(spec.width * spec.height));
    for (int y = 0; y < spec.height; ++y)
        for (int x = 0; x < spec.width; ++x)
            out.push_back(spec.palette[spec.indices[static_cast<std::size_t>(y * spec.width + x)]]);
    return out;
}

std::string error_of(const std::vector<std::uint8_t> &bytes) {
    return load_tpl(bytes).error;
}

void single_4bpp_texture() {
    const TextureSpec spec = clut4_spec();
    const std::vector<std::uint8_t> container = build_tpl({spec});
    const Tpl tpl = load_tpl(container);
    check(tpl.ok, tpl.error.c_str());
    check(tpl.bytes == container, "original container bytes are preserved for the writer");
    check(tpl.textures.size() == 1 && tpl.resources.empty(), "one texture, no resources");
    const TplTexture &texture = tpl.textures[0];
    check(texture.state.format == ge::TextureFormat::Clut4, "CLUT4 pixel format");
    check(texture.state.width == 6 && texture.state.height == 8, "odd width and height");
    check(texture.state.buffer_width == 32, "buffer width is preserved");
    check(texture.state.swizzled, "TPL texels are swizzled");
    check(texture.palette_entries == 16 && texture.palette_format == 3, "16-entry 8888 palette");
    check(texture.state.clut_format == ((15u << 8u) | 3u), "CLUT mask selects all 16 entries");
    check(texture.data.size() == 32u * 8u / 2u, "data size follows buffer width and 4bpp");
    check(texture.palette.size() == 16u * 4u, "palette data size follows entry count");
    const std::vector<std::uint32_t> pixels = decode_texture(texture);
    check(pixels == expected_pixels(spec), "4bpp alpha ramp decodes exactly");
}

void single_4bpp_palette565() {
    const TextureSpec spec = clut4_palette565_spec();
    const Tpl tpl = load_tpl(build_tpl({spec}));
    check(tpl.ok, tpl.error.c_str());
    const TplTexture &texture = tpl.textures[0];
    check(texture.palette_format == 0 && texture.palette_entries == 16, "RGB565 palette header");
    check(texture.state.clut_format == ((15u << 8u) | 0u), "RGB565 palette keeps its format bits");
    check(decode_texture(texture) == expected_pixels(spec), "RGB565 palette expands exactly");
}

void single_8bpp_texture() {
    const TextureSpec spec = clut8_spec();
    const Tpl tpl = load_tpl(build_tpl({spec}));
    check(tpl.ok, tpl.error.c_str());
    check(tpl.textures.size() == 1, "one CLUT8 texture");
    const TplTexture &texture = tpl.textures[0];
    check(texture.state.format == ge::TextureFormat::Clut8, "CLUT8 pixel format");
    check(texture.state.width == 4 && texture.state.height == 8, "8bpp dimensions");
    check(texture.state.buffer_width == 16, "8bpp buffer width");
    check(texture.palette_entries == 256, "256-entry palette");
    check(texture.state.clut_format == ((255u << 8u) | 3u), "CLUT mask selects all 256 entries");
    check(texture.data.size() == 16u * 8u, "data size follows buffer width and 8bpp");
    const std::vector<std::uint32_t> pixels = decode_texture(texture);
    check(pixels == expected_pixels(spec), "8bpp CLUT decodes exactly");
}

void multi_texture_container() {
    TextureSpec first = clut4_spec();
    TextureSpec second = clut8_spec();
    second.width = 8;
    second.height = 8;
    second.buffer_width = 16;
    second.indices.assign(64, 0);
    for (int i = 0; i < 64; ++i) second.indices[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    TextureSpec third = clut4_spec();
    third.width = 4;
    third.height = 8;
    third.buffer_width = 32;
    third.indices.assign(32, 7);
    const Tpl tpl = load_tpl(build_tpl({first, second, third}));
    check(tpl.ok, tpl.error.c_str());
    check(tpl.declared_textures == 3 && tpl.textures.size() == 3, "three textures");
    check(tpl.textures[0].header_offset != tpl.textures[1].header_offset, "headers are distinct");
    check(tpl.textures[0].palette_header_offset != tpl.textures[1].palette_header_offset,
          "palettes are distinct");
    check(tpl.textures[0].state.format == ge::TextureFormat::Clut4, "first format");
    check(tpl.textures[1].state.format == ge::TextureFormat::Clut8, "second format");
    check(tpl.textures[2].state.format == ge::TextureFormat::Clut4, "third format");
    check(decode_texture(tpl.textures[0]) == expected_pixels(first), "first texture pixels");
    check(decode_texture(tpl.textures[1]) == expected_pixels(second), "second texture pixels");
    check(decode_texture(tpl.textures[2]) == expected_pixels(third), "third texture pixels");
}

void resource_entry_is_counted() {
    TextureSpec texture = clut4_spec();
    TextureSpec resource;
    resource.resource = true;
    resource.resource_bytes = {8, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    const Tpl tpl = load_tpl(build_tpl({texture, resource}));
    check(tpl.ok, tpl.error.c_str());
    check(tpl.declared_textures == 2, "declared entry count includes the resource");
    check(tpl.textures.size() == 1, "only the real texture is decoded");
    check(tpl.resources.size() == 1, "the non-texture entry is counted, not dropped");
    check(tpl.resources[0].pixel_format == kResourcePixelFormat, "resource sentinel is preserved");
    check(tpl.resources[0].data_size == resource.resource_bytes.size(), "resource size is bounded");
}

void malformed_inputs_are_named() {
    const std::vector<std::uint8_t> valid = build_tpl({clut4_spec()});

    check(load_tpl(std::span<const std::uint8_t>{}).error == "truncated_header",
          "empty input is rejected");
    check(error_of(std::vector<std::uint8_t>(4, 0)) == "truncated_header", "short header is rejected");

    std::vector<std::uint8_t> bad = valid;
    write_u32(bad, 0, 0);
    check(error_of(bad) == "invalid_texture_count", "zero entries are rejected");
    bad = valid;
    write_u32(bad, 0, 65);
    check(error_of(bad) == "invalid_texture_count", "oversized entry table is rejected");
    bad = valid;
    write_u32(bad, 4, 0x0C);
    check(error_of(bad) == "invalid_table_offset", "non-standard table offset is rejected");

    bad = valid;
    write_u32(bad, 8, static_cast<std::uint32_t>(bad.size()));
    check(error_of(bad) == "texture_header_out_of_range", "header offset is bounds-checked");

    bad = valid;
    write_u32(bad, 0x10 + 8, static_cast<std::uint32_t>(bad.size()));
    check(error_of(bad) == "data_out_of_range", "data range is bounds-checked");

    bad = valid;
    write_u16(bad, 0x24, 0);
    check(error_of(bad) == "invalid_palette_entries", "zero-entry palette is rejected");
    bad = valid;
    write_u16(bad, 0x24, 3);
    check(error_of(bad) == "invalid_palette_entries", "non-power-of-two palette is rejected");

    bad = valid;
    write_u32(bad, 0x24 + 4, static_cast<std::uint32_t>(bad.size()));
    check(error_of(bad) == "palette_out_of_range", "palette range is bounds-checked");

    bad = valid;
    write_u16(bad, 0x10 + 6, 8);
    check(error_of(bad) == "unsupported_pixel_format", "DXT format is named as unsupported");
    bad = valid;
    write_u16(bad, 0x10 + 6, 0x0108);
    check(error_of(bad) == "unsupported_pixel_format", "unknown format word is named");

    bad = valid;
    write_u32(bad, 0x0C, 0);
    check(error_of(bad) == "missing_palette", "CLUT texture without a palette is rejected");

    bad = valid;
    write_u16(bad, 0x10, 0);
    check(error_of(bad) == "invalid_texture_dimensions", "zero height is rejected");
    bad = valid;
    write_u16(bad, 0x10 + 4, 4);
    check(error_of(bad) == "invalid_buffer_width", "buffer width below width is rejected");
    bad = valid;
    write_u16(bad, 0x10 + 4, 8);
    check(error_of(bad) == "invalid_swizzle_geometry", "partial swizzle rows are rejected");
    bad = valid;
    write_u16(bad, 0x10, 7);
    check(error_of(bad) == "invalid_swizzle_geometry", "partial swizzle blocks are rejected");
}

} // namespace

int main() {
    try {
        single_4bpp_texture();
        single_4bpp_palette565();
        single_8bpp_texture();
        multi_texture_container();
        resource_entry_is_counted();
        malformed_inputs_are_named();
        std::cout << "tex_tpl_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

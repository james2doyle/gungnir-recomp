#include "tex/tpl.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace gungnir::tex {
namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]) |
           static_cast<std::uint16_t>(bytes[offset + 1]) << 8u;
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset]) |
           static_cast<std::uint32_t>(bytes[offset + 1]) << 8u |
           static_cast<std::uint32_t>(bytes[offset + 2]) << 16u |
           static_cast<std::uint32_t>(bytes[offset + 3]) << 24u;
}

Tpl fail(Tpl result, const char *error) {
    result.ok = false;
    result.error = error;
    return result;
}

bool is_clut(std::uint16_t format) {
    return format == static_cast<std::uint16_t>(ge::TextureFormat::Clut4) ||
           format == static_cast<std::uint16_t>(ge::TextureFormat::Clut8) ||
           format == static_cast<std::uint16_t>(ge::TextureFormat::Clut16) ||
           format == static_cast<std::uint16_t>(ge::TextureFormat::Clut32);
}

bool is_power_of_two(std::uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

} // namespace

Tpl load_tpl(std::span<const std::uint8_t> bytes) {
    Tpl result;
    result.bytes.assign(bytes.begin(), bytes.end());
    if (bytes.size() < 8) return fail(std::move(result), "truncated_header");

    result.declared_textures = read_u32(bytes, 0);
    result.table_offset = read_u32(bytes, 4);
    if (result.declared_textures == 0 || result.declared_textures > 64)
        return fail(std::move(result), "invalid_texture_count");
    if (result.table_offset != 8) return fail(std::move(result), "invalid_table_offset");
    const std::uint64_t table_bytes =
        static_cast<std::uint64_t>(result.declared_textures) * 8u;
    if (table_bytes > bytes.size() - result.table_offset)
        return fail(std::move(result), "truncated_entry_table");

    result.table_entries.resize(result.declared_textures);
    for (std::uint32_t index = 0; index < result.declared_textures; ++index) {
        const std::size_t entry = result.table_offset + static_cast<std::size_t>(index) * 8u;
        std::memcpy(result.table_entries[index].data(), bytes.data() + entry, 8);
    }

    std::vector<std::uint32_t> chunk_starts;
    for (std::uint32_t index = 0; index < result.declared_textures; ++index) {
        const std::size_t entry = result.table_offset + static_cast<std::size_t>(index) * 8u;
        const std::uint32_t header_offset = read_u32(bytes, entry);
        const std::uint32_t palette_header_offset = read_u32(bytes, entry + 4);
        if (static_cast<std::uint64_t>(header_offset) + 0x14u > bytes.size())
            return fail(std::move(result), "texture_header_out_of_range");

        const std::uint16_t height = read_u16(bytes, header_offset);
        const std::uint16_t width = read_u16(bytes, header_offset + 2);
        const std::uint16_t buffer_width = read_u16(bytes, header_offset + 4);
        const std::uint16_t pixel_format = read_u16(bytes, header_offset + 6);
        const std::uint32_t data_offset = read_u32(bytes, header_offset + 8);

        if (pixel_format == kResourcePixelFormat) {
            if (data_offset > bytes.size()) return fail(std::move(result), "resource_out_of_range");
            TplResource resource;
            resource.header_offset = header_offset;
            resource.data_offset = data_offset;
            resource.pixel_format = pixel_format;
            result.resources.push_back(resource);
            chunk_starts.push_back(data_offset);
            continue;
        }

        if (pixel_format > static_cast<std::uint16_t>(ge::TextureFormat::Clut32))
            return fail(std::move(result), "unsupported_pixel_format");
        if (height == 0 || width == 0) return fail(std::move(result), "invalid_texture_dimensions");
        if (buffer_width < width) return fail(std::move(result), "invalid_buffer_width");

        const ge::TextureFormat format = static_cast<ge::TextureFormat>(pixel_format);
        const int bits = ge::texture_bits_per_texel(format);
        if (bits <= 0) return fail(std::move(result), "unsupported_pixel_format");
        const std::uint64_t row_bits = static_cast<std::uint64_t>(buffer_width) *
                                       static_cast<std::uint64_t>(bits);
        if (row_bits % 8u != 0) return fail(std::move(result), "invalid_buffer_width");
        const std::uint64_t row_bytes = row_bits / 8u;
        if (row_bytes % 16u != 0 || height % 8u != 0)
            return fail(std::move(result), "invalid_swizzle_geometry");
        const std::uint64_t data_size = row_bytes * height;
        if (data_offset > bytes.size() || data_size > bytes.size() - data_offset)
            return fail(std::move(result), "data_out_of_range");

        TplTexture texture;
        texture.header_offset = header_offset;
        texture.palette_header_offset = palette_header_offset;
        texture.data_offset = data_offset;
        std::memcpy(texture.header.data(), bytes.data() + header_offset, texture.header.size());
        texture.state.format = format;
        texture.state.swizzled = true;
        texture.state.width = width;
        texture.state.height = height;
        texture.state.buffer_width = buffer_width;
        texture.data.assign(bytes.begin() + data_offset, bytes.begin() + data_offset + data_size);

        if (palette_header_offset != 0) {
            if (static_cast<std::uint64_t>(palette_header_offset) + 8u > bytes.size())
                return fail(std::move(result), "palette_header_out_of_range");
            const std::uint16_t entries = read_u16(bytes, palette_header_offset);
            const std::uint16_t palette_format = read_u16(bytes, palette_header_offset + 2);
            const std::uint32_t palette_data_offset = read_u32(bytes, palette_header_offset + 4);
            if (palette_format > 3) return fail(std::move(result), "unsupported_palette_format");
            if (!is_power_of_two(entries) || entries > 256)
                return fail(std::move(result), "invalid_palette_entries");
            const std::uint64_t entry_bytes = palette_format == 3 ? 4u : 2u;
            const std::uint64_t palette_size = static_cast<std::uint64_t>(entries) * entry_bytes;
            if (palette_data_offset > bytes.size() ||
                palette_size > bytes.size() - palette_data_offset)
                return fail(std::move(result), "palette_out_of_range");

            texture.palette_entries = entries;
            texture.palette_format = palette_format;
            texture.palette_data_offset = palette_data_offset;
            std::memcpy(texture.palette_header.data(), bytes.data() + palette_header_offset,
                        texture.palette_header.size());
            texture.palette.assign(bytes.begin() + palette_data_offset,
                                   bytes.begin() + palette_data_offset + palette_size);
            texture.state.clut_format =
                (static_cast<std::uint32_t>(palette_format) & 3u) |
                ((static_cast<std::uint32_t>(entries) - 1u) << 8u);
            chunk_starts.push_back(palette_data_offset);
        } else if (is_clut(pixel_format)) {
            return fail(std::move(result), "missing_palette");
        }

        chunk_starts.push_back(data_offset);
        result.textures.push_back(std::move(texture));
    }

    std::sort(chunk_starts.begin(), chunk_starts.end());
    for (TplResource &resource : result.resources) {
        const auto next = std::upper_bound(chunk_starts.begin(), chunk_starts.end(),
                                          resource.data_offset);
        const std::uint32_t end = next == chunk_starts.end()
                                      ? static_cast<std::uint32_t>(bytes.size())
                                      : *next;
        resource.data_size = end >= resource.data_offset ? end - resource.data_offset : 0u;
    }

    result.ok = true;
    return result;
}

Tpl load_tpl_file(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        Tpl result;
        result.error = "file_not_found";
        return result;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    return load_tpl(bytes);
}

std::vector<std::uint32_t> decode_texture(const TplTexture &texture) {
    const ge::Texture decoded(texture.state, texture.data, texture.palette);
    std::vector<std::uint32_t> pixels;
    decoded.decode(pixels);
    return pixels;
}

} // namespace gungnir::tex

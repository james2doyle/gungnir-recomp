#pragma once

// TPL container decoding (T8a). A TPL holds one or more PSP textures plus
// opaque auxiliary resources. The model preserves the raw header bytes so the
// T8b writer can reproduce the container byte-for-byte.

#include "ge/texture.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gungnir::tex {

inline constexpr std::uint16_t kResourcePixelFormat = 0xFFFFu;

struct TplTexture {
    ge::TextureState state;
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> palette;
    std::uint32_t header_offset{};
    std::uint32_t palette_header_offset{};
    std::uint32_t data_offset{};
    std::uint32_t palette_data_offset{};
    std::uint16_t palette_entries{};
    std::uint16_t palette_format{};
    std::array<std::uint8_t, 0x14> header{};
    std::array<std::uint8_t, 8> palette_header{};
};

struct TplResource {
    std::uint32_t header_offset{};
    std::uint32_t data_offset{};
    std::uint32_t data_size{};
    std::uint16_t pixel_format{};
    std::array<std::uint8_t, 0x14> header{};
};

struct Tpl {
    std::vector<std::uint8_t> bytes;
    std::uint32_t declared_textures{};
    std::uint32_t table_offset{};
    std::vector<std::array<std::uint8_t, 8>> table_entries;
    std::vector<TplTexture> textures;
    std::vector<TplResource> resources;
    std::string error;
    bool ok{false};
};

Tpl load_tpl(std::span<const std::uint8_t> bytes);
Tpl load_tpl_file(const std::string &path);
std::vector<std::uint32_t> decode_texture(const TplTexture &texture);

struct TplWriteResult {
    std::vector<std::uint8_t> bytes;
    std::string error;
    bool ok{false};
};

// In-place container writer: offsets and the original padding are preserved.
// Writing an unmodified model reproduces the input byte-for-byte; changing
// texture/palette bytes changes exactly those byte ranges.
TplWriteResult write_tpl(const Tpl &tpl);

} // namespace gungnir::tex

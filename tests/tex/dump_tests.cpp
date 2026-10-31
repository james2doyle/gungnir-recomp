// T8c contract tests: content keys, PNG dump and textures.ini manifest.
//
// Every texture below is hand-authored: linear texel data, synthetic palettes.

#include "tex/dump.hpp"
#include "tex/png.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir;
using namespace gungnir::tex;
namespace fs = std::filesystem;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void append_u32(std::vector<std::uint8_t> &data, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        data.push_back(static_cast<std::uint8_t>(value >> shift));
}

TplTexture make_texture(int width, int height, int buffer_width, ge::TextureFormat format,
                        const std::vector<std::uint8_t> &indices,
                        const std::vector<std::uint32_t> &palette) {
    TplTexture texture;
    texture.state.format = format;
    texture.state.swizzled = false;
    texture.state.width = width;
    texture.state.height = height;
    texture.state.buffer_width = buffer_width;
    const int bits = ge::texture_bits_per_texel(format);
    const int row_bytes = buffer_width * bits / 8;
    texture.data.assign(static_cast<std::size_t>(row_bytes) * static_cast<std::size_t>(height), 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const std::uint8_t raw =
                indices[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                        static_cast<std::size_t>(x)];
            const std::uint8_t index =
                bits == 4 ? static_cast<std::uint8_t>(raw & 15u) : raw;
            const std::size_t offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(row_bytes) +
                                       static_cast<std::size_t>(x) * static_cast<std::size_t>(bits) / 8u;
            if (bits == 4)
                texture.data[offset] = static_cast<std::uint8_t>(
                    texture.data[offset] | (x % 2 == 0 ? index : index << 4u));
            else
                texture.data[offset] = index;
        }
    texture.palette_entries = static_cast<std::uint16_t>(palette.size());
    texture.palette_format = 3;
    for (const std::uint32_t color : palette) append_u32(texture.palette, color);
    texture.state.clut_format = ((static_cast<std::uint32_t>(palette.size()) - 1u) << 8u) | 3u;
    return texture;
}

std::vector<std::uint32_t> clut4_palette() {
    std::vector<std::uint32_t> palette(16);
    for (int i = 0; i < 16; ++i)
        palette[static_cast<std::size_t>(i)] =
            ge::rgba8(255u, 255u, 255u, static_cast<std::uint32_t>(i) * 17u);
    return palette;
}

TplTexture clut4_texture() {
    std::vector<std::uint8_t> indices(6u * 4u);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 6; ++x)
            indices[static_cast<std::size_t>(y * 6 + x)] =
                static_cast<std::uint8_t>((x + y * 6) % 16);
    return make_texture(6, 4, 6, ge::TextureFormat::Clut4, indices, clut4_palette());
}

std::vector<std::uint8_t> rgba_bytes(const std::vector<std::uint32_t> &pixels) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(pixels.size() * 4u);
    for (const std::uint32_t pixel : pixels) {
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<std::uint8_t>(pixel >> shift));
    }
    return bytes;
}

fs::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
                          ("gungnir_tex_dump_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    return path;
}

std::string read_text(const fs::path &path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::size_t count_substring(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

void key_is_content_addressed() {
    const TplTexture texture = clut4_texture();
    const std::string key = texture_key(texture);
    check(key.size() == 16, "key is 16 hex digits");
    check(std::all_of(key.begin(), key.end(), [](char c) {
              return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
          }),
          "key is uppercase hexadecimal");
    check(texture_key(texture) == key, "key is stable");

    TplTexture changed = texture;
    changed.palette[1] = static_cast<std::uint8_t>(changed.palette[1] ^ 0xFFu);
    check(texture_key(changed) != key, "CLUT bytes change the key");

    changed = texture;
    changed.data[0] = static_cast<std::uint8_t>(changed.data[0] ^ 0xFFu);
    check(texture_key(changed) != key, "texel bytes change the key");

    changed = texture;
    changed.state.buffer_width += 2;
    check(texture_key(changed) != key, "buffer width changes the key");

    changed = texture;
    changed.state.swizzled = true;
    check(texture_key(changed) != key, "swizzle state changes the key");

    changed = texture;
    changed.state.format = ge::TextureFormat::Clut8;
    check(texture_key(changed) != key, "pixel format changes the key");

    changed = texture;
    changed.state.linear = true;
    changed.state.clamp_u = true;
    changed.state.clamp_v = true;
    check(texture_key(changed) == key, "live sampler state does not change the content key");
}

void png_round_trip_and_errors() {
    const fs::path directory = make_temp_dir();
    fs::create_directories(directory);
    const std::vector<std::uint8_t> pixels = {255, 0, 0, 255, 0, 255, 0, 128,
                                              0, 0, 255, 64, 255, 255, 255, 0};
    const fs::path path = directory / "roundtrip.png";
    const PngWriteResult written = write_png(path, 2, 2, pixels);
    check(written.ok, written.error.c_str());
    const PngReadResult read = read_png(path);
    check(read.ok, read.error.c_str());
    check(read.image.width == 2 && read.image.height == 2, "PNG dimensions round-trip");
    check(read.image.rgba == pixels, "PNG pixels round-trip");

    check(write_png(path, 0, 2, pixels).error == "invalid_image", "invalid PNG size is rejected");
    check(write_png(path, 2, 2, std::span<const std::uint8_t>(pixels.data(), 8)).error ==
              "invalid_image",
          "short pixel buffer is rejected");
    check(read_png(directory / "missing.png").error == "file_not_found",
          "missing PNG is named");
    std::ofstream bad(directory / "bad.png");
    bad << "not a png";
    bad.close();
    check(read_png(directory / "bad.png").error == "png_read_failed", "corrupt PNG is named");

    std::error_code error;
    fs::remove_all(directory, error);
}

void dump_writes_png_and_manifest() {
    const TplTexture texture = clut4_texture();
    const std::string key = texture_key(texture);
    const fs::path directory = make_temp_dir();
    const DumpResult first = dump_textures(std::span<const TplTexture>(&texture, 1), {directory});
    check(first.ok, first.error.c_str());
    check(first.written == 1 && first.skipped == 0, "first dump writes one PNG");
    check(first.entries.size() == 1 && first.entries[0].key == key, "entry carries the key");
    const fs::path png_path = directory / "new" / (key + ".png");
    check(fs::exists(png_path), "PNG is written under new/");

    const PngReadResult png = read_png(png_path);
    check(png.ok, png.error.c_str());
    check(png.image.width == 6 && png.image.height == 4, "PNG dimensions match the texture");
    check(png.image.rgba == rgba_bytes(decode_texture(texture)), "PNG pixels match the decode");

    const std::string manifest = read_text(directory / "textures.ini");
    check(manifest.find("[options]") != std::string::npos, "manifest has options");
    check(manifest.find("version = 1") != std::string::npos, "manifest version");
    check(manifest.find("hash = gungnir64") != std::string::npos, "manifest hash name");
    check(manifest.find("[hashes]") != std::string::npos, "manifest has hashes section");
    check(manifest.find(key + " = new/" + key + ".png") != std::string::npos,
          "manifest maps the key to its PNG");
    check(count_substring(manifest, key + " = new/" + key + ".png") == 1,
          "manifest has one entry for the key");

    const DumpResult second = dump_textures(std::span<const TplTexture>(&texture, 1), {directory});
    check(second.ok, second.error.c_str());
    check(second.written == 0 && second.skipped == 1, "repeat dump skips the existing PNG");
    check(count_substring(read_text(directory / "textures.ini"),
                          key + " = new/" + key + ".png") == 1,
          "repeat dump does not duplicate the manifest entry");

    std::error_code error;
    fs::remove_all(directory, error);
}

void dump_multiple_textures() {
    TplTexture first = clut4_texture();
    TplTexture second = first;
    second.palette[0] = static_cast<std::uint8_t>(second.palette[0] ^ 0xFFu);
    const std::array<TplTexture, 2> textures{first, second};
    const fs::path directory = make_temp_dir();
    const DumpResult result = dump_textures(textures, {directory});
    check(result.ok, result.error.c_str());
    check(result.written == 2 && result.entries.size() == 2, "batch dump writes every texture");
    check(result.entries[0].key != result.entries[1].key, "batch entries keep distinct keys");
    const std::string manifest = read_text(directory / "textures.ini");
    for (const DumpEntry &entry : result.entries) {
        check(fs::exists(directory / entry.filename), "batch PNG exists");
        check(count_substring(manifest, entry.key + " = " + entry.filename) == 1,
              "batch manifest entry exists once");
    }
    std::error_code error;
    fs::remove_all(directory, error);
}

void clut_change_creates_second_dump() {
    const TplTexture texture = clut4_texture();
    const fs::path directory = make_temp_dir();
    check(dump_textures(std::span<const TplTexture>(&texture, 1), {directory}).ok,
          "base texture dumps");

    TplTexture changed = texture;
    changed.palette[0] = static_cast<std::uint8_t>(changed.palette[0] ^ 0xFFu);
    const DumpResult result = dump_textures(std::span<const TplTexture>(&changed, 1), {directory});
    check(result.ok, result.error.c_str());
    check(result.written == 1 && result.entries[0].key == texture_key(changed),
          "changed CLUT dumps under a new key");
    check(result.entries[0].key != texture_key(texture), "changed CLUT has a different key");
    const std::string manifest = read_text(directory / "textures.ini");
    const std::string old_key = texture_key(texture);
    const std::string new_key = texture_key(changed);
    check(count_substring(manifest, old_key + " = new/" + old_key + ".png") == 1,
          "old key remains");
    check(count_substring(manifest, new_key + " = new/" + new_key + ".png") == 1,
          "new key is appended");

    std::error_code error;
    fs::remove_all(directory, error);
}

void existing_mismatch_is_named() {
    const TplTexture texture = clut4_texture();
    const std::string key = texture_key(texture);
    const fs::path directory = make_temp_dir();
    check(dump_textures(std::span<const TplTexture>(&texture, 1), {directory}).ok,
          "base texture dumps");

    const fs::path png_path = directory / "new" / (key + ".png");
    const std::vector<std::uint8_t> black(6u * 4u * 4u, 0);
    check(write_png(png_path, 6, 4, black).ok, "collision fixture writes");
    const DumpResult collision = dump_textures(std::span<const TplTexture>(&texture, 1), {directory});
    check(!collision.ok && collision.error == "key_collision", "different existing PNG is a collision");
    const PngReadResult after = read_png(png_path);
    check(after.ok && after.image.rgba == black, "collision does not overwrite the existing PNG");

    const fs::path corrupt_dir = make_temp_dir();
    check(dump_textures(std::span<const TplTexture>(&texture, 1), {corrupt_dir}).ok,
          "corrupt fixture dumps");
    std::ofstream corrupt(corrupt_dir / "new" / (key + ".png"));
    corrupt << "not a png";
    corrupt.close();
    const DumpResult invalid = dump_textures(std::span<const TplTexture>(&texture, 1), {corrupt_dir});
    check(!invalid.ok && invalid.error == "existing_png_invalid", "corrupt existing PNG is named");

    std::error_code error;
    fs::remove_all(directory, error);
    fs::remove_all(corrupt_dir, error);
}

void invalid_dump_inputs_are_named() {
    const TplTexture texture = clut4_texture();
    const fs::path directory = make_temp_dir();
    const DumpResult empty = dump_textures(std::span<const TplTexture>{}, {directory});
    check(!empty.ok && empty.error == "no_textures", "empty texture set is named");
    const DumpResult no_root = dump_textures(std::span<const TplTexture>(&texture, 1), {});
    check(!no_root.ok && no_root.error == "invalid_output_directory", "empty output root is named");
    Tpl invalid_tpl;
    const DumpResult bad_tpl = dump_tpl(invalid_tpl, {directory});
    check(!bad_tpl.ok && bad_tpl.error == "invalid_tpl", "invalid TPL is named");

    Tpl tpl;
    tpl.ok = true;
    tpl.textures.push_back(texture);
    const DumpResult wrapped = dump_tpl(tpl, {directory});
    check(wrapped.ok && wrapped.written == 1, "dump_tpl dumps its textures");

    std::error_code error;
    fs::remove_all(directory, error);
}

} // namespace

int main() {
    try {
        key_is_content_addressed();
        png_round_trip_and_errors();
        dump_writes_png_and_manifest();
        dump_multiple_textures();
        clut_change_creates_second_dump();
        existing_mismatch_is_named();
        invalid_dump_inputs_are_named();
        std::cout << "tex_dump_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

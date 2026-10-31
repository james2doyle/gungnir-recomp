// T8d contract tests: replacement lookup, precedence, scaling and fallback.
//
// Replacements and native textures are hand-authored; no game assets.

#include "ge/software.hpp"
#include "tex/dump.hpp"
#include "tex/png.hpp"
#include "tex/replace.hpp"

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
using namespace gungnir::ge;
namespace fs = std::filesystem;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

TplTexture native_texture() {
    TplTexture texture;
    texture.state.format = ge::TextureFormat::Rgba8888;
    texture.state.swizzled = false;
    texture.state.width = 2;
    texture.state.height = 2;
    texture.state.buffer_width = 2;
    texture.state.clamp_u = true;
    texture.state.clamp_v = true;
    texture.data = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    return texture;
}

std::vector<std::uint8_t> upscale_nearest(const std::vector<std::uint8_t> &rgba, int width, int height,
                                          int scale) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width * scale) *
                                      static_cast<std::size_t>(height * scale) * 4u);
    for (int y = 0; y < height * scale; ++y)
        for (int x = 0; x < width * scale; ++x) {
            const int source = ((y / scale) * width + (x / scale)) * 4;
            const int target = (y * width * scale + x) * 4;
            std::copy_n(rgba.data() + source, 4, out.data() + target);
        }
    return out;
}

fs::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path = fs::temp_directory_path() /
                          ("gungnir_tex_replace_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    fs::create_directories(path / "replace");
    return path;
}

ScreenVertex vertex(float x, float y) {
    ScreenVertex out;
    out.x = x;
    out.y = y;
    out.z = 0.5f;
    out.inv_w = 1.0f;
    return out;
}

ge::Framebuffer render_replacement(const ReplacementImage &image, const ge::TextureState &original,
                                   int size) {
    ge::Framebuffer framebuffer(size, size);
    framebuffer.clear(ge::rgba8(0, 0, 0));
    const ge::TextureState state = image.state_for(original);
    const ge::Texture texture(state, image.rgba);
    ge::RasterState raster;
    raster.texture_enabled = true;
    raster.texture = &texture;
    ge::SoftwareRasterizer software(framebuffer, raster);
    ScreenVertex bottom_right = vertex(static_cast<float>(size), static_cast<float>(size));
    bottom_right.texcoord = {1.0f, 1.0f, 1.0f};
    software.draw_sprite(vertex(0.0f, 0.0f), bottom_right);
    return framebuffer;
}

ge::Framebuffer render_native(const TplTexture &native) {
    ge::Framebuffer framebuffer(2, 2);
    framebuffer.clear(ge::rgba8(0, 0, 0));
    const ge::Texture texture(native.state, native.data);
    ge::RasterState raster;
    raster.texture_enabled = true;
    raster.texture = &texture;
    ge::SoftwareRasterizer software(framebuffer, raster);
    ScreenVertex bottom_right = vertex(2.0f, 2.0f);
    bottom_right.texcoord = {1.0f, 1.0f, 1.0f};
    software.draw_sprite(vertex(0.0f, 0.0f), bottom_right);
    return framebuffer;
}

void check_block_pattern(const ge::Framebuffer &framebuffer) {
    const std::array<std::uint32_t, 4> expected = {
        ge::rgba8(255, 0, 0), ge::rgba8(0, 255, 0), ge::rgba8(0, 0, 255), ge::rgba8(255, 255, 255)};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const std::uint32_t want = expected[static_cast<std::size_t>((y / 2) * 2 + (x / 2))];
            check(framebuffer.color_at(x, y) == want, "replacement blocks reach the frame");
        }
}

void replacement_scene_at_2x_and_4x() {
    const TplTexture native = native_texture();
    const std::string key = texture_key(native);
    for (const int scale : {2, 4}) {
        const fs::path directory = make_temp_dir();
        const std::vector<std::uint8_t> upscaled =
            upscale_nearest(native.data, 2, 2, scale);
        const PngWriteResult written =
            write_png(directory / "replace" / (key + ".png"), 2 * scale, 2 * scale, upscaled);
        check(written.ok, written.error.c_str());

        TextureReplacements replacements(directory);
        const ReplacementLookup found = replacements.find(key);
        check(found.ok, found.error.c_str());
        check(found.image->width == 2 * scale && found.image->height == 2 * scale,
              "replacement keeps any resolution");
        check(found.image->rgba == upscaled, "replacement pixels load exactly");

        const ge::TextureState state = found.image->state_for(native.state);
        check(state.format == ge::TextureFormat::Rgba8888 && !state.swizzled,
              "replacement samples as linear RGBA8");
        check(state.width == 2 * scale && state.height == 2 * scale &&
                  state.buffer_width == 2 * scale,
              "replacement state uses the image dimensions");
        check(state.clamp_u && state.clamp_v && !state.linear,
              "replacement state copies live clamp/filter state");

        check_block_pattern(render_replacement(*found.image, native.state, 4));

        const ge::Framebuffer native_frame = render_native(native);
        check(native_frame.color_at(0, 0) == ge::rgba8(255, 0, 0) &&
                  native_frame.color_at(1, 0) == ge::rgba8(0, 255, 0) &&
                  native_frame.color_at(0, 1) == ge::rgba8(0, 0, 255) &&
                  native_frame.color_at(1, 1) == ge::rgba8(255, 255, 255),
              "native texture remains the fallback");

        std::error_code error;
        fs::remove_all(directory, error);
    }
}

void scan_reports_unmatched_and_errors() {
    const TplTexture native = native_texture();
    const std::string key = texture_key(native);
    const fs::path directory = make_temp_dir();
    const std::vector<std::uint8_t> upscaled = upscale_nearest(native.data, 2, 2, 2);
    check(write_png(directory / "replace" / (key + ".png"), 4, 4, upscaled).ok,
          "known replacement writes");
    check(write_png(directory / "replace" / "0000000000000000.png", 4, 4, upscaled).ok,
          "unknown replacement writes");
    TextureReplacements replacements(directory);
    const std::array<std::string, 1> known = {key};
    const ReplacementScan scan = replacements.scan(known);
    check(scan.present == 2 && scan.loaded == 1, "scan counts PNGs and loaded keys");
    check(scan.unmatched.size() == 1 && scan.unmatched[0] == "0000000000000000.png",
          "scan names the unmatched PNG");
    check(scan.errors.empty(), "scan has no errors for valid files");

    const fs::path corrupt_dir = make_temp_dir();
    std::ofstream corrupt(corrupt_dir / "replace" / (key + ".png"));
    corrupt << "not a png";
    corrupt.close();
    TextureReplacements corrupt_store(corrupt_dir);
    const ReplacementLookup missing = corrupt_store.find(key);
    check(!missing.ok && missing.error == "png_read_failed", "corrupt replacement fails closed");
    const ReplacementScan corrupt_scan = corrupt_store.scan(known);
    check(corrupt_scan.present == 1 && corrupt_scan.loaded == 0, "corrupt scan counts a present PNG");
    check(corrupt_scan.errors.size() == 1 &&
              corrupt_scan.errors[0] == key + ".png: png_read_failed",
          "corrupt scan names the error");

    check(replacements.find("FFFFFFFFFFFFFFFF").error == "not_found",
          "key mismatch is not_found");
    check(replacements.find("xyz").error == "invalid_key", "invalid key is named");
    TextureReplacements empty_root;
    check(empty_root.find(key).error == "invalid_root", "empty root is named");

    std::error_code error;
    fs::remove_all(directory, error);
    fs::remove_all(corrupt_dir, error);
}

void cache_clear_reloads() {
    const TplTexture native = native_texture();
    const std::string key = texture_key(native);
    const fs::path directory = make_temp_dir();
    const std::vector<std::uint8_t> two_x = upscale_nearest(native.data, 2, 2, 2);
    const std::vector<std::uint8_t> four_x = upscale_nearest(native.data, 2, 2, 4);
    check(write_png(directory / "replace" / (key + ".png"), 4, 4, two_x).ok, "2x writes");
    TextureReplacements replacements(directory);
    const ReplacementLookup first = replacements.find(key);
    check(first.ok && first.image->width == 4, "first lookup caches the 2x image");
    check(write_png(directory / "replace" / (key + ".png"), 8, 8, four_x).ok, "4x overwrites");
    const ReplacementLookup cached = replacements.find(key);
    check(cached.ok && cached.image->width == 4, "lookup stays cached until clear");
    replacements.clear();
    const ReplacementLookup reloaded = replacements.find(key);
    check(reloaded.ok && reloaded.image->width == 8, "clear reloads the replacement");

    std::error_code error;
    fs::remove_all(directory, error);
}

void dump_does_not_touch_replacements() {
    const TplTexture native = native_texture();
    const std::string key = texture_key(native);
    const fs::path directory = make_temp_dir();
    const std::span<const TplTexture> textures(&native, 1);
    check(dump_textures(textures, {directory}).ok, "base dump writes");
    const std::vector<std::uint8_t> replacement = upscale_nearest(native.data, 2, 2, 2);
    check(write_png(directory / "replace" / (key + ".png"), 4, 4, replacement).ok,
          "replacement writes");
    const PngReadResult before = read_png(directory / "replace" / (key + ".png"));
    check(before.ok, before.error.c_str());
    check(dump_textures(textures, {directory}).ok, "repeat dump writes");
    const PngReadResult after = read_png(directory / "replace" / (key + ".png"));
    check(after.ok, after.error.c_str());
    check(after.image.rgba == before.image.rgba, "dump never overwrites a user replacement");

    std::error_code error;
    fs::remove_all(directory, error);
}

void scan_without_replace_directory() {
    const fs::path directory = make_temp_dir();
    std::error_code error;
    fs::remove_all(directory / "replace", error);
    TextureReplacements replacements(directory);
    const std::array<std::string, 1> known = {texture_key(native_texture())};
    const ReplacementScan scan = replacements.scan(known);
    check(scan.present == 0 && scan.loaded == 0 && scan.unmatched.empty() && scan.errors.empty(),
          "missing replace directory scans empty");
    fs::remove_all(directory, error);
}

} // namespace

int main() {
    try {
        replacement_scene_at_2x_and_4x();
        scan_reports_unmatched_and_errors();
        cache_clear_reloads();
        dump_does_not_touch_replacements();
        scan_without_replace_directory();
        std::cout << "tex_replace_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

// T7g contract tests: deterministic post-processing presets and hashes.
//
// The source image is a hand-authored 2x2 fixture; no game frames are used.

#include "post/post.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace gungnir::post;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::uint32_t rgba8(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a = 255u) {
    return r | (g << 8u) | (b << 16u) | (a << 24u);
}

Image fixture() {
    Image image;
    image.width = 2;
    image.height = 2;
    image.pixels = {rgba8(255, 0, 0), rgba8(0, 255, 0), rgba8(0, 0, 255), rgba8(255, 255, 255)};
    return image;
}

void default_is_noop() {
    const Image source = fixture();
    const Image result = apply(source, default_preset());
    check(result == source, "default preset is a no-op");
    check(hash(result) == hash(source), "default preset preserves the hash");
}

void integer_scale_is_exact() {
    const Image source = fixture();
    LoadResult loaded = load_preset(R"({"name":"pixel","passes":[{"type":"integer_scale","factor":2}]})");
    check(loaded.ok, loaded.error.c_str());
    const Image scaled = apply(source, loaded.preset);
    check(scaled.width == 4 && scaled.height == 4, "integer scale doubles the extent");
    const std::uint32_t expected[16] = {
        source.pixels[0], source.pixels[0], source.pixels[1], source.pixels[1],
        source.pixels[0], source.pixels[0], source.pixels[1], source.pixels[1],
        source.pixels[2], source.pixels[2], source.pixels[3], source.pixels[3],
        source.pixels[2], source.pixels[2], source.pixels[3], source.pixels[3],
    };
    for (int i = 0; i < 16; ++i) check(scaled.pixels[static_cast<std::size_t>(i)] == expected[i],
                                       "integer scale replicates each texel exactly");
}

void screen_presets() {
    const Image source = fixture();

    LoadResult scanlines = load_preset(
        R"({"name":"crt","passes":[{"type":"integer_scale","factor":2},{"type":"scanlines","strength":0.5,"period":2}]})");
    check(scanlines.ok, scanlines.error.c_str());
    const Image scanline_image = apply(source, scanlines.preset);
    check(scanline_image.width == 4 && scanline_image.height == 4, "scanline preset keeps the scaled extent");
    for (int y = 0; y < 2; ++y) {
        const std::uint32_t pixel = scanline_image.pixels[static_cast<std::size_t>(y) * 4u];
        const std::uint32_t red = (pixel >> 0u) & 255u;
        check(red == (y % 2 == 0 ? 128u : 255u), "scanlines darken every other source row");
    }

    LoadResult grid = load_preset(
        R"({"name":"lcd","passes":[{"type":"integer_scale","factor":2},{"type":"lcd_grid","strength":0.5,"period":2}]})");
    check(grid.ok, grid.error.c_str());
    const Image grid_image = apply(source, grid.preset);
    check(((grid_image.pixels[0] >> 0u) & 255u) == 128u, "LCD grid darkens the first column and row");
    check(((grid_image.pixels[15] >> 0u) & 255u) == 255u, "LCD grid leaves the interior texel alone");

    LoadResult fsr = load_preset(
        R"({"name":"fsr","passes":[{"type":"fsr_lite","scale":2,"sharpness":0.5}]})");
    check(fsr.ok, fsr.error.c_str());
    const Image fsr_image = apply(source, fsr.preset);
    check(fsr_image.width == 4 && fsr_image.height == 4, "FSR-lite doubles the extent");
    check(hash(fsr_image) != hash(source), "FSR-lite changes the readback");
}

void invalid_presets_are_named() {
    LoadResult result = load_preset("{");
    check(!result.ok && result.error == "invalid_json", "malformed JSON is rejected by name");
    result = load_preset("{}");
    check(!result.ok && result.error == "missing_passes", "missing passes are rejected by name");
    result = load_preset(R"({"passes":[{"type":"warp"}]})");
    check(!result.ok && result.error == "unknown_pass", "unknown pass is rejected by name");
    result = load_preset(R"({"passes":[{"type":"integer_scale","factor":0}]})");
    check(!result.ok && result.error == "invalid_factor", "invalid scale factor is rejected by name");
    result = load_preset(R"({"passes":[{"type":"scanlines","strength":2.0}]})");
    check(!result.ok && result.error == "invalid_strength", "invalid strength is rejected by name");
}

void pinned_hashes() {
    const Image source = fixture();
    const auto load = [](const char *text) {
        LoadResult result = load_preset(text);
        check(result.ok, result.error.c_str());
        return result.preset;
    };
    const Image pixel = apply(source, load(R"({"name":"pixel","passes":[{"type":"integer_scale","factor":2}]})"));
    const Image scanlines = apply(source, load(
        R"({"name":"crt","passes":[{"type":"integer_scale","factor":2},{"type":"scanlines","strength":0.5,"period":2}]})"));
    const Image grid = apply(source, load(
        R"({"name":"lcd","passes":[{"type":"integer_scale","factor":2},{"type":"lcd_grid","strength":0.5,"period":2}]})"));
    const Image fsr = apply(source, load(R"({"name":"fsr","passes":[{"type":"fsr_lite","scale":2,"sharpness":0.5}]})"));
    check(hash(pixel) != hash(source), "integer preset changes the readback");
    check(hash(scanlines) != hash(pixel), "scanline preset changes the readback");
    check(hash(grid) != hash(pixel), "LCD preset changes the readback");
    check(hash(fsr) != hash(source), "FSR preset changes the readback");
    check(hash(pixel) == 0x7C16FEB54D10C625ull, "pixel preset hash is pinned");
    check(hash(scanlines) == 0xE39B854A7C56CB15ull, "scanline preset hash is pinned");
    check(hash(grid) == 0x7B801CF1B64A6785ull, "LCD preset hash is pinned");
    check(hash(fsr) == 0xC42D01FDE37FF8F1ull, "FSR preset hash is pinned");
}

} // namespace

int main() {
    try {
        default_is_noop();
        integer_scale_is_exact();
        screen_presets();
        invalid_presets_are_named();
        pinned_hashes();
        std::cout << "post_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

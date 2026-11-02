// T15x contract tests: per-target surfaces, display selection and
// render-to-texture sampling from target surfaces.

#include "ge/targets.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace gungnir::ge;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

ScreenVertex vertex(float x, float y, std::uint32_t color) {
    ScreenVertex out;
    out.x = x;
    out.y = y;
    out.z = 0.5f;
    out.inv_w = 1.0f;
    out.color = {static_cast<float>(color & 255u) / 255.0f,
                 static_cast<float>((color >> 8u) & 255u) / 255.0f,
                 static_cast<float>((color >> 16u) & 255u) / 255.0f,
                 static_cast<float>((color >> 24u) & 255u) / 255.0f};
    return out;
}

void full_screen(SoftwareRasterizer &rasterizer, std::uint32_t color) {
    const ScreenVertex a = vertex(-100.0f, -100.0f, color);
    const ScreenVertex b = vertex(100.0f, -100.0f, color);
    const ScreenVertex c = vertex(0.0f, 100.0f, color);
    rasterizer.draw_triangle(a, b, c);
}

void routing() {
    TargetSet targets(32, 32);
    {
        TargetEntry &entry = targets.draw_to(0x100u, TextureFormat::Rgba8888);
        SoftwareRasterizer rasterizer(entry.surface);
        full_screen(rasterizer, rgba8(255, 0, 0));
        entry.dirty = true;
    }
    {
        TargetEntry &entry = targets.draw_to(0x200u, TextureFormat::Rgba8888);
        SoftwareRasterizer rasterizer(entry.surface);
        full_screen(rasterizer, rgba8(0, 255, 0));
        entry.dirty = true;
    }
    check(targets.has(0x100u) && targets.has(0x200u), "both targets exist");
    check(targets.draw_to(0x100u, TextureFormat::Rgba8888).surface.color_at(0, 0) == rgba8(255, 0, 0), "target 0x100 holds red");
    check(targets.draw_to(0x200u, TextureFormat::Rgba8888).surface.color_at(0, 0) == rgba8(0, 255, 0), "target 0x200 holds green");
    const Framebuffer *selected = targets.display(0x200u);
    check(selected != nullptr && selected->color_at(0, 0) == rgba8(0, 255, 0),
          "display select returns the matching target");
    const Framebuffer *fallback = targets.display(0x999u);
    check(fallback != nullptr && fallback->color_at(0, 0) == rgba8(255, 0, 0),
          "unknown display buffer falls back to target 0");
}

void rtt_sampling() {
    TargetSet targets(32, 32);
    {
        TargetEntry &entry = targets.draw_to(0u, TextureFormat::Rgba8888);
        SoftwareRasterizer rasterizer(entry.surface);
        full_screen(rasterizer, rgba8(255, 0, 0));
        entry.dirty = true;
    }
    TextureState state;
    state.format = TextureFormat::Rgba8888;
    state.width = 32;
    state.height = 32;
    state.buffer_width = 32;
    auto sampled = targets.sample_target(kVramBase, state);
    check(sampled.has_value(), "exact-match target samples from the surface");
    if (sampled.has_value())
        check(sampled->texture->sample(0.02f, 0.02f) == rgba8(255, 0, 0),
              "sampled texel is the drawn red");
    check(!targets.sample_target(0x08000000u, state).has_value(),
          "RAM texture does not sample surfaces");
    TextureState narrow = state;
    narrow.format = TextureFormat::Rgb565;
    check(!targets.sample_target(kVramBase, narrow).has_value(),
          "mismatched format falls back to RAM");
    TextureState clean;
    clean.format = TextureFormat::Rgba8888;
    clean.width = 32;
    clean.height = 32;
    clean.buffer_width = 32;
    TargetSet fresh(32, 32);
    fresh.draw_to(0u, TextureFormat::Rgba8888);
    check(!fresh.sample_target(kVramBase, clean).has_value(),
          "clean target does not override RAM");
}

void fb_format_5650() {
    // 16-bit targets quantize on write: (128, 128, 128) becomes
    // (131, 129, 131) through the 565 pack/unpack round-trip.
    TargetSet targets(32, 32);
    TargetEntry &entry = targets.draw_to(0u, TextureFormat::Rgb565);
    RasterState state;
    state.fb_format = TextureFormat::Rgb565;
    SoftwareRasterizer rasterizer(entry.surface, state);
    const ScreenVertex a = vertex(-100.0f, -100.0f, rgba8(128, 128, 128));
    const ScreenVertex b = vertex(100.0f, -100.0f, rgba8(128, 128, 128));
    const ScreenVertex c = vertex(0.0f, 100.0f, rgba8(128, 128, 128));
    rasterizer.draw_triangle(a, b, c);
    check(targets.draw_to(0u, TextureFormat::Rgb565).surface.color_at(0, 0) == rgba8(131, 129, 131),
          "5650 target quantizes gray");
}

} // namespace

int main() {
    try {
        routing();
        rtt_sampling();
        fb_format_5650();
        std::cout << "ge_targets_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

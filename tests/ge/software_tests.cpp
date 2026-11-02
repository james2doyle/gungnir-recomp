// T7c contract tests: software rasterizer primitives, depth, blend, scissor,
// alpha test, stencil-lite, texture sampling and deterministic readback.

#include "ge/software.hpp"
#include "ge/texture.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir::ge;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::uint32_t channel(std::uint32_t value, int shift) {
    return (value >> shift) & 255u;
}

ScreenVertex vertex(float x, float y, float z = 0.5f, std::uint32_t color = 0xFFFFFFFFu) {
    ScreenVertex out;
    out.x = x;
    out.y = y;
    out.z = z;
    out.inv_w = 1.0f;
    out.color = {static_cast<float>(channel(color, 0)) / 255.0f, static_cast<float>(channel(color, 8)) / 255.0f,
                 static_cast<float>(channel(color, 16)) / 255.0f, static_cast<float>(channel(color, 24)) / 255.0f};
    return out;
}

void full_screen(SoftwareRasterizer &rasterizer, std::uint32_t color, float z = 0.5f) {
    const ScreenVertex a = vertex(-100.0f, -100.0f, z, color);
    const ScreenVertex b = vertex(100.0f, -100.0f, z, color);
    const ScreenVertex c = vertex(0.0f, 100.0f, z, color);
    rasterizer.draw_triangle(a, b, c);
}

void triangle_contract() {
    Framebuffer framebuffer(4, 4);
    framebuffer.clear(rgba8(0, 0, 0));
    SoftwareRasterizer rasterizer(framebuffer);
    rasterizer.draw_triangle(vertex(0.0f, 0.0f), vertex(4.0f, 0.0f), vertex(0.0f, 4.0f));
    check(framebuffer.color_at(0, 0) == rgba8(255, 255, 255), "pixel (0,0) is inside the triangle");
    check(framebuffer.color_at(1, 1) == rgba8(255, 255, 255), "pixel (1,1) is inside the triangle");
    check(framebuffer.color_at(2, 2) == rgba8(0, 0, 0), "pixel (2,2) is outside the triangle");
    check(framebuffer.color_at(3, 0) == rgba8(255, 255, 255), "the x-axis edge is included");
    check(framebuffer.color_at(0, 3) == rgba8(255, 255, 255), "the y-axis edge is included");
    check(framebuffer.color_at(3, 3) == rgba8(0, 0, 0), "the far corner stays clear");
}

void strip_fan_and_sprites() {
    Framebuffer framebuffer(4, 4);
    framebuffer.clear(rgba8(0, 0, 0));
    SoftwareRasterizer rasterizer(framebuffer);
    const std::vector<ScreenVertex> strip = {
        vertex(0.0f, 0.0f), vertex(4.0f, 0.0f), vertex(0.0f, 4.0f), vertex(4.0f, 4.0f)};
    rasterizer.draw_primitive(Primitive::TriangleStrip, strip);
    check(framebuffer.color_at(0, 0) != rgba8(0, 0, 0), "strip covers the first half");
    check(framebuffer.color_at(3, 3) != rgba8(0, 0, 0), "strip covers the second half");

    framebuffer.clear(rgba8(0, 0, 0));
    const std::vector<ScreenVertex> fan = {
        vertex(0.0f, 0.0f), vertex(4.0f, 0.0f), vertex(4.0f, 4.0f), vertex(0.0f, 4.0f)};
    rasterizer.draw_primitive(Primitive::TriangleFan, fan);
    check(framebuffer.color_at(1, 1) != rgba8(0, 0, 0), "fan covers the first quadrant");
    check(framebuffer.color_at(3, 1) != rgba8(0, 0, 0), "fan covers the second quadrant");

    framebuffer.clear(rgba8(0, 0, 0));
    rasterizer.draw_sprite(vertex(0.0f, 0.0f), vertex(4.0f, 4.0f));
    check(framebuffer.color_at(0, 0) == rgba8(255, 255, 255), "sprite covers its top-left");
    check(framebuffer.color_at(3, 3) == rgba8(255, 255, 255), "sprite covers its bottom-right");
}

void depth_ordering() {
    Framebuffer framebuffer(2, 2);
    framebuffer.clear(rgba8(0, 0, 0), 0xFFFFu);
    RasterState state;
    state.depth_test = true;
    state.depth_write = true;
    state.depth_function = CompareFunc::Less;
    SoftwareRasterizer rasterizer(framebuffer, state);
    full_screen(rasterizer, rgba8(255, 0, 0), 100.0f);
    full_screen(rasterizer, rgba8(0, 0, 255), 50.0f);
    check(framebuffer.color_at(0, 0) == rgba8(0, 0, 255), "nearer fragment wins");
    check(framebuffer.depth_at(0, 0) == 50u, "depth buffer stores the nearer value");

    framebuffer.clear(rgba8(0, 0, 0), 0xFFFFu);
    full_screen(rasterizer, rgba8(0, 0, 255), 50.0f);
    full_screen(rasterizer, rgba8(255, 0, 0), 100.0f);
    check(framebuffer.color_at(0, 0) == rgba8(0, 0, 255), "farther fragment is rejected");
}

void blending() {
    Framebuffer framebuffer(1, 1);
    framebuffer.clear(rgba8(0, 0, 0));
    RasterState state;
    state.blend.enabled = true;
    state.blend.source = BlendFactor::One;
    state.blend.destination = BlendFactor::One;
    SoftwareRasterizer rasterizer(framebuffer, state);
    full_screen(rasterizer, rgba8(255, 0, 0));
    full_screen(rasterizer, rgba8(0, 255, 0));
    const std::uint32_t additive = framebuffer.color_at(0, 0);
    check(channel(additive, 0) == 255u && channel(additive, 8) == 255u && channel(additive, 16) == 0u,
          "additive One/One blends red and green");

    framebuffer.clear(rgba8(0, 0, 0));
    rasterizer.state().blend.source = BlendFactor::SrcAlpha;
    rasterizer.state().blend.destination = BlendFactor::OneMinusSrcAlpha;
    full_screen(rasterizer, rgba8(255, 0, 0, 128));
    check(channel(framebuffer.color_at(0, 0), 0) == 128u,
          "source-alpha blend produces the hand-computed half red");

    // PSP modulate: source * destination. White over red stays red;
    // red over blue goes black.
    framebuffer.clear(rgba8(255, 0, 0));
    rasterizer.state().blend.source = BlendFactor::DstColor;
    rasterizer.state().blend.destination = BlendFactor::Zero;
    full_screen(rasterizer, rgba8(255, 255, 255));
    check(framebuffer.color_at(0, 0) == rgba8(255, 0, 0), "other-color source modulates");
    framebuffer.clear(rgba8(0, 0, 255));
    full_screen(rasterizer, rgba8(255, 0, 0));
    check(framebuffer.color_at(0, 0) == rgba8(0, 0, 0), "disjoint modulate goes black");

    // FIX reads the latched BLENDFIXED register color: green FIX over
    // anything yields green.
    framebuffer.clear(rgba8(255, 0, 0));
    rasterizer.state().blend.source = BlendFactor::Fix;
    rasterizer.state().blend.destination = BlendFactor::Zero;
    rasterizer.state().blend.fixed_source = 0x0000FF00u;
    full_screen(rasterizer, rgba8(255, 255, 255));
    check(framebuffer.color_at(0, 0) == rgba8(0, 255, 0), "FIX source applies the fixed color");

    // Doubled source alpha clamps: 128 doubled saturates, so red wins fully.
    framebuffer.clear(rgba8(0, 0, 0));
    rasterizer.state().blend.source = BlendFactor::DoubleSrcAlpha;
    rasterizer.state().blend.destination = BlendFactor::Zero;
    full_screen(rasterizer, rgba8(255, 0, 0, 128));
    check(framebuffer.color_at(0, 0) == rgba8(255, 0, 0, 128), "doubled source alpha saturates");

    // Additive overflow clamps instead of wrapping mod 256 (both reference
    // impls clamp). White over white stays white instead of wrapping.
    framebuffer.clear(rgba8(0, 0, 0));
    rasterizer.state().blend.source = BlendFactor::One;
    rasterizer.state().blend.destination = BlendFactor::One;
    rasterizer.state().blend.equation = BlendEquation::Add;
    full_screen(rasterizer, rgba8(255, 255, 255));
    check(framebuffer.color_at(0, 0) == rgba8(255, 255, 255, 255), "additive blend clamps at white");
    // blue keeps blue's alpha; a full-keep mask leaves the target alone.
    framebuffer.clear(rgba8(0, 0, 255, 128));
    rasterizer.state().blend.enabled = false;
    rasterizer.state().write_mask = 0xFF000000u;
    full_screen(rasterizer, rgba8(255, 0, 0, 255));
    check(framebuffer.color_at(0, 0) == rgba8(255, 0, 0, 128), "write mask keeps the masked channels");
    rasterizer.state().write_mask = 0xFFFFFFFFu;
    full_screen(rasterizer, rgba8(0, 255, 0, 255));
    check(framebuffer.color_at(0, 0) == rgba8(255, 0, 0, 128), "full write mask is a no-op");
}

void scissor_and_alpha_test() {
    Framebuffer framebuffer(4, 4);
    framebuffer.clear(rgba8(0, 0, 0));
    RasterState state;
    state.scissor_x0 = 1;
    state.scissor_y0 = 1;
    state.scissor_x1 = 3;
    state.scissor_y1 = 3;
    SoftwareRasterizer rasterizer(framebuffer, state);
    full_screen(rasterizer, rgba8(255, 255, 255));
    check(framebuffer.color_at(0, 0) == rgba8(0, 0, 0), "scissor rejects the top-left");
    check(framebuffer.color_at(1, 1) == rgba8(255, 255, 255), "scissor includes its origin");
    check(framebuffer.color_at(2, 2) == rgba8(255, 255, 255), "scissor includes its last column and row");
    check(framebuffer.color_at(3, 3) == rgba8(0, 0, 0), "scissor rejects outside its rectangle");

    framebuffer.clear(rgba8(0, 0, 0));
    state.scissor_x0 = 0;
    state.scissor_y0 = 0;
    state.scissor_x1 = 4;
    state.scissor_y1 = 4;
    state.alpha_test = true;
    state.alpha_function = CompareFunc::Greater;
    state.alpha_reference = 200;
    rasterizer.state() = state;
    full_screen(rasterizer, rgba8(0, 255, 0, 128));
    check(framebuffer.color_at(1, 1) == rgba8(0, 0, 0), "alpha test rejects a fragment below the reference");
    state.alpha_reference = 100;
    rasterizer.state() = state;
    full_screen(rasterizer, rgba8(0, 255, 0, 128));
    check(framebuffer.color_at(1, 1) == rgba8(0, 255, 0, 128), "alpha test keeps a fragment above the reference");
}

void fog_mix() {
    // Fog mixes toward the fog color by the per-vertex factor (1 = no fog).
    // Full fog turns white into the fog color; no fog leaves it alone.
    Framebuffer framebuffer(1, 1);
    framebuffer.clear(rgba8(0, 0, 0));
    RasterState state;
    state.fog_enabled = true;
    state.fog_color = 0x0000FF00u;
    SoftwareRasterizer rasterizer(framebuffer, state);
    ScreenVertex a = vertex(-100.0f, -100.0f, 0.5f, rgba8(255, 255, 255));
    ScreenVertex b = vertex(100.0f, -100.0f, 0.5f, rgba8(255, 255, 255));
    ScreenVertex c = vertex(0.0f, 100.0f, 0.5f, rgba8(255, 255, 255));
    a.fog = b.fog = c.fog = 0.0f;
    rasterizer.draw_triangle(a, b, c);
    check(framebuffer.color_at(0, 0) == rgba8(0, 255, 0), "full fog yields the fog color");
    framebuffer.clear(rgba8(0, 0, 0));
    a.fog = b.fog = c.fog = 1.0f;
    rasterizer.draw_triangle(a, b, c);
    check(framebuffer.color_at(0, 0) == rgba8(255, 255, 255), "no fog keeps the source");
}

void stencil_lite() {
    Framebuffer framebuffer(2, 2);
    framebuffer.clear(rgba8(0, 0, 0), 0xFFFFu, 0u);
    RasterState state;
    state.scissor_x1 = 2;
    state.scissor_y1 = 2;
    state.stencil.test = true;
    state.stencil.write = true;
    state.stencil.function = CompareFunc::Always;
    state.stencil.reference = 1;
    SoftwareRasterizer rasterizer(framebuffer, state);
    full_screen(rasterizer, rgba8(255, 0, 0));
    check(framebuffer.stencil_at(0, 0) == 1u && framebuffer.color_at(0, 0) == rgba8(255, 0, 0),
          "stencil write marks the pass");
    state.stencil.function = CompareFunc::Equal;
    rasterizer.state() = state;
    full_screen(rasterizer, rgba8(0, 255, 0));
    check(framebuffer.color_at(0, 0) == rgba8(0, 255, 0), "equal stencil reference passes");
    state.stencil.reference = 2;
    rasterizer.state() = state;
    full_screen(rasterizer, rgba8(0, 0, 255));
    check(framebuffer.color_at(0, 0) == rgba8(0, 255, 0), "mismatched stencil reference rejects");
}

void texture_sampling() {
    std::vector<std::uint8_t> data(2u * 2u * 4u);
    const std::uint32_t colors[4] = {rgba8(255, 0, 0), rgba8(0, 255, 0), rgba8(0, 0, 255), rgba8(255, 255, 255)};
    for (int i = 0; i < 4; ++i) {
        data[static_cast<std::size_t>(i) * 4u] = static_cast<std::uint8_t>(colors[i] & 0xFFu);
        data[static_cast<std::size_t>(i) * 4u + 1u] = static_cast<std::uint8_t>((colors[i] >> 8u) & 0xFFu);
        data[static_cast<std::size_t>(i) * 4u + 2u] = static_cast<std::uint8_t>((colors[i] >> 16u) & 0xFFu);
        data[static_cast<std::size_t>(i) * 4u + 3u] = static_cast<std::uint8_t>((colors[i] >> 24u) & 0xFFu);
    }
    TextureState texture_state;
    texture_state.format = TextureFormat::Rgba8888;
    texture_state.width = 2;
    texture_state.height = 2;
    texture_state.buffer_width = 2;
    texture_state.clamp_u = true;
    texture_state.clamp_v = true;
    const Texture texture(texture_state, data);

    Framebuffer framebuffer(2, 2);
    framebuffer.clear(rgba8(0, 0, 0));
    RasterState state;
    state.texture_enabled = true;
    state.texture = &texture;
    SoftwareRasterizer rasterizer(framebuffer, state);
    ScreenVertex top_left = vertex(0.0f, 0.0f);
    top_left.texcoord = {0.0f, 0.0f, 1.0f};
    ScreenVertex bottom_right = vertex(2.0f, 2.0f);
    bottom_right.texcoord = {1.0f, 1.0f, 1.0f};
    rasterizer.draw_sprite(top_left, bottom_right);
    check(framebuffer.color_at(0, 0) == rgba8(255, 0, 0), "texture samples the top-left texel");
    check(framebuffer.color_at(1, 0) == rgba8(0, 255, 0), "texture samples the top-right texel");
    check(framebuffer.color_at(0, 1) == rgba8(0, 0, 255), "texture samples the bottom-left texel");
    check(framebuffer.color_at(1, 1) == rgba8(255, 255, 255), "texture samples the bottom-right texel");
}

void golden_and_determinism() {
    const auto render = []() {
        Framebuffer framebuffer(8, 8);
        framebuffer.clear(rgba8(8, 8, 16), 0xFFFFu);
        RasterState state;
        state.depth_test = true;
        state.depth_write = true;
        state.depth_function = CompareFunc::Less;
        SoftwareRasterizer rasterizer(framebuffer, state);
        ScreenVertex a = vertex(0.5f, 0.5f, 10.0f, rgba8(255, 0, 0));
        ScreenVertex b = vertex(7.5f, 0.5f, 20.0f, rgba8(0, 255, 0));
        ScreenVertex c = vertex(0.5f, 7.5f, 30.0f, rgba8(0, 0, 255));
        rasterizer.draw_triangle(a, b, c);
        return framebuffer.hash();
    };
    const std::uint64_t first = render();
    const std::uint64_t second = render();
    check(first == second, "two runs produce the same framebuffer hash");
    check(first == 0x422B77A53737E005ull, "golden scene matches the pinned readback hash");
}

} // namespace

int main() {
    try {
        triangle_contract();
        strip_fan_and_sprites();
        depth_ordering();
        blending();
        scissor_and_alpha_test();
        stencil_lite();
        fog_mix();
        texture_sampling();
        golden_and_determinism();
        std::cout << "ge_software_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

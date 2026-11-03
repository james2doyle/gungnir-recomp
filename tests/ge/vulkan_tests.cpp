// T7e contract tests: embedded shaders, headless Vulkan triangle readback and
// windowed swapchain creation/recreation.

#include "ge/vulkan_backend.hpp"
#include "ge/vulkan_shaders.hpp"

#include "ge/software.hpp"
#include "ge/targets.hpp"
#include "ge/texture.hpp"

#if defined(GUNGNIR_HAVE_SDL3)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir::ge;

void check(bool condition, const std::string &contract) {
    if (!condition) throw std::runtime_error(contract);
}

void shader_validation() {
    check(shaders::triangle_vert_size > 4u && shaders::triangle_frag_size > 4u, "both shader modules are embedded");
    check(shaders::triangle_vert[0] == 0x07230203u && shaders::triangle_frag[0] == 0x07230203u,
          "shader modules start with the SPIR-V magic");
}

void viewport_fitting() {
    const ViewportRect same = fit_viewport(480, 272, 480, 272);
    check(same.x == 0 && same.y == 0 && same.width == 480 && same.height == 272,
          "an equal target keeps the guest viewport");

    const ViewportRect wide = fit_viewport(480, 272, 3063, 1335);
    check(wide.width == 2356 && wide.height == 1335 && wide.x == 353 && wide.y == 0,
          "a wider surface pillar-boxes a 16:9 guest without stretching");

    const ViewportRect tall = fit_viewport(480, 272, 272, 480);
    check(tall.width == 272 && tall.height == 154 && tall.x == 0 && tall.y == 163,
          "a taller surface letter-boxes a 16:9 guest without stretching");

    const ViewportRect degenerate = fit_viewport(0, 272, 480, 272);
    check(degenerate.width == 0 && degenerate.height == 0, "a zero guest extent yields an empty viewport");
}

void scissor_scaling() {
    const ScissorRect full = scale_scissor_to_extent({0, 0, 480, 272}, 480, 272, 3063, 1335);
    check(full.x0 == 0 && full.y0 == 0 && full.x1 == 3063 && full.y1 == 1335,
          "a full guest scissor covers the full swapchain");

    const ScissorRect identity = scale_scissor_to_extent({10, 20, 110, 120}, 480, 272, 480, 272);
    check(identity.x0 == 10 && identity.y0 == 20 && identity.x1 == 110 && identity.y1 == 120,
          "a scissor at the guest extent is unchanged");

    const ScissorRect band = scale_scissor_to_extent({0, 103, 480, 169}, 480, 272, 3063, 1335);
    check(band.x0 == 0 && band.x1 == 3063, "a full-width band scales to the full swapchain width");
    check(band.y0 == 505 && band.y1 == 830, "scissor edges round outward to the target extent");

    const ScissorRect empty = scale_scissor_to_extent({200, 100, 100, 50}, 480, 272, 3063, 1335);
    check(empty.x1 <= empty.x0 || empty.y1 <= empty.y0, "an inverted scissor stays empty");

    const ScissorRect invalid = scale_scissor_to_extent({0, 0, 480, 272}, 0, 272, 3063, 1335);
    check(invalid.x1 <= invalid.x0 || invalid.y1 <= invalid.y0, "a non-positive source extent yields an empty scissor");
}

void headless_triangle() {
    std::string reason;
    if (!VulkanBackend::available(reason)) {
        std::cout << "SKIP: " << reason << "\n";
        std::exit(77);
    }

    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 64;
    config.height = 64;
    config.headless = true;
    std::string error;
    check(backend.initialize(config, error), error.c_str());
    const gungnir::platform::VulkanDriverInfo &driver = backend.driver_info();
    check(driver.loaded, "driver info is loaded after initialization");
    check(!driver.driver_name.empty(), "driver name is reported");
    check(driver.api_version >= VK_API_VERSION_1_1, "driver API version is reported");

    VulkanFrame first;
    check(backend.render_headless(first, error), error.c_str());
    check(first.width == 64 && first.height == 64 && first.rgba.size() == 64u * 64u * 4u,
          "headless readback has the requested extent");
    bool varied = false;
    for (std::size_t i = 0; i < first.rgba.size(); i += 4) {
        check(first.rgba[i + 3] == 255u, "headless triangle is opaque");
        if (first.rgba[i] != 0u || first.rgba[i + 1] != 0u || first.rgba[i + 2] != 0u) varied = true;
    }
    check(varied, "headless triangle wrote colour pixels");

    VulkanFrame second;
    check(backend.render_headless(second, error), error.c_str());
    check(first.rgba == second.rgba, "two headless frames are byte-identical");
    backend.shutdown();
}

struct Quad {
    std::array<VulkanDrawVertex, 4> vk;
    std::array<ScreenVertex, 4> sw;
    std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
};

float channel_float(std::uint32_t color, int shift) {
    return static_cast<float>((color >> shift) & 255u) / 255.0f;
}

Quad make_quad(const std::array<std::uint32_t, 4> &colors, float z = 0.5f, bool textured = false) {
    const std::array<std::array<float, 2>, 4> positions{{{0.0f, 0.0f}, {32.0f, 0.0f}, {32.0f, 32.0f}, {0.0f, 32.0f}}};
    const std::array<std::array<float, 2>, 4> texcoords{{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
    Quad quad;
    for (std::size_t i = 0; i < 4; ++i) {
        const std::array<float, 4> color{channel_float(colors[i], 0), channel_float(colors[i], 8),
                                         channel_float(colors[i], 16), channel_float(colors[i], 24)};
        quad.vk[i] = {positions[i][0], positions[i][1], z, textured ? texcoords[i][0] : 0.0f,
                      textured ? texcoords[i][1] : 0.0f, color[0], color[1], color[2], color[3]};
        quad.sw[i] = {};
        quad.sw[i].x = positions[i][0];
        quad.sw[i].y = positions[i][1];
        quad.sw[i].z = z;
        quad.sw[i].inv_w = 1.0f;
        quad.sw[i].texcoord = {quad.vk[i].u, quad.vk[i].v, 1.0f};
        quad.sw[i].color = color;
    }
    return quad;
}

void draw_quad_software(SoftwareRasterizer &rasterizer, const Quad &quad) {
    rasterizer.draw_triangle(quad.sw[0], quad.sw[1], quad.sw[2]);
    rasterizer.draw_triangle(quad.sw[0], quad.sw[2], quad.sw[3]);
}

void compare_frames(const Framebuffer &software, const VulkanFrame &vulkan, int tolerance, bool ignore_alpha,
                    const char *contract) {
    check(software.width() == vulkan.width && software.height() == vulkan.height, "software and Vulkan extents match");
    for (int y = 0; y < software.height(); ++y) {
        for (int x = 0; x < software.width(); ++x) {
            const std::uint32_t expected = software.color_at(x, y);
            const std::size_t offset = (static_cast<std::size_t>(y) * software.width() + x) * 4u;
            for (int c = 0; c < 4; ++c) {
                if (ignore_alpha && c == 3) continue;
                const int actual = vulkan.rgba[offset + c];
                const int wanted = static_cast<int>((expected >> (c * 8)) & 255u);
                if (std::abs(actual - wanted) > tolerance) {
                    std::cerr << "PARITY " << contract << " at " << x << "," << y << " channel " << c
                              << " expected " << wanted << " got " << actual << "\n";
                    throw std::runtime_error(contract);
                }
            }
        }
    }
}

void depth_format_tests() {
    // Fallback chain is data, not driver queries: D32F+S8 first, then D24S8.
    const auto all = [](VkFormat) { return true; };
    check(choose_depth_format(all) == VK_FORMAT_D32_SFLOAT_S8_UINT, "D32F+S8 preferred");
    const auto no32 = [](VkFormat format) { return format != VK_FORMAT_D32_SFLOAT_S8_UINT; };
    check(choose_depth_format(no32) == VK_FORMAT_D24_UNORM_S8_UINT, "D24S8 fallback");
    const auto none = [](VkFormat) { return false; };
    check(choose_depth_format(none) == VK_FORMAT_UNDEFINED, "no depth format reports undefined");
}

void sampler_sharing_tests() {
    std::string reason;
    if (!VulkanBackend::available(reason)) {
        std::cout << "SKIP: " << reason << "\n";
        std::exit(77);
    }
    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 32;
    config.height = 32;
    config.headless = true;
    std::string error;
    check(backend.initialize(config, error), error.c_str());
    const VkSampler linear_clamp = backend.sampler_for(true, true, true);
    check(linear_clamp != VK_NULL_HANDLE, "shared sampler created");
    check(backend.sampler_for(true, true, true) == linear_clamp, "same params share one sampler");
    check(backend.sampler_for(true, false, false) != linear_clamp, "repeat differs from clamp");
    check(backend.sampler_for(false, true, true) != linear_clamp, "nearest differs from linear");
    check(backend.sampler_for(true, true, false) != linear_clamp, "split clamp axes differ");
    backend.shutdown();
}

void parity_tests() {
    std::string reason;
    if (!VulkanBackend::available(reason)) {
        std::cout << "SKIP: " << reason << "\n";
        std::exit(77);
    }
    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 32;
    config.height = 32;
    config.headless = true;
    std::string error;
    check(backend.initialize(config, error), error.c_str());

    const std::array<std::uint32_t, 4> colors{rgba8(255, 0, 0), rgba8(0, 255, 0), rgba8(0, 0, 255),
                                              rgba8(255, 255, 255)};
    const Quad solid = make_quad(colors);
    Framebuffer software(32, 32);
    software.clear(rgba8(0, 0, 0));
    SoftwareRasterizer software_rasterizer(software);
    draw_quad_software(software_rasterizer, solid);
    VulkanFrame vulkan;
    const VulkanDrawState solid_state;
    check(backend.render_headless_draw(solid.vk, solid.indices, solid_state, vulkan, error, 0u), error.c_str());
    compare_frames(software, vulkan, 1, false, "solid colour quad matches software");
    const std::size_t first_pipeline_count = backend.pipeline_count();
    check(first_pipeline_count == 1u, "one pipeline for one draw state");
    check(backend.render_headless_draw(solid.vk, solid.indices, solid_state, vulkan, error, 0u), error.c_str());
    check(backend.pipeline_count() == first_pipeline_count, "repeated state reuses the pipeline");
    VulkanDrawState cull_state;
    cull_state.cull = CullMode::Back;
    cull_state.front_face = FrontFace::Clockwise;
    check(backend.render_headless_draw(solid.vk, solid.indices, cull_state, vulkan, error, 0u), error.c_str());
    check(backend.pipeline_count() == first_pipeline_count + 1u, "cull state creates one additional pipeline");

    const Quad near = make_quad({rgba8(0, 0, 255), rgba8(0, 0, 255), rgba8(0, 0, 255), rgba8(0, 0, 255)}, 50.0f);
    const Quad far = make_quad({rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0)}, 100.0f);
    Framebuffer depth_software(32, 32);
    depth_software.clear(rgba8(0, 0, 0), 0xFFFFu);
    RasterState depth_state;
    depth_state.depth_test = true;
    depth_state.depth_write = true;
    depth_state.depth_function = CompareFunc::Less;
    SoftwareRasterizer depth_rasterizer(depth_software, depth_state);
    draw_quad_software(depth_rasterizer, far);
    draw_quad_software(depth_rasterizer, near);
    VulkanDrawState vk_depth;
    vk_depth.depth_test = true;
    vk_depth.depth_write = true;
    vk_depth.depth_function = CompareFunc::Less;
    const std::array<VulkanDrawCall, 2> depth_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(far.vk), far.indices, vk_depth},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(near.vk), near.indices, vk_depth}};
    check(backend.render_headless_draws(depth_calls, vulkan, error, 0u), error.c_str());
    compare_frames(depth_software, vulkan, 1, false, "depth ordering matches software");

    // Clear-write vs clear-keep parity (depth-clear audit lock): the captured
    // corpus clears depth to 0 every frame and uses bit-10 keeps that must
    // leave the plane untouched. Observed through a GREATER-tested probe quad
    // so the plane value decides the pixel colour on both backends.
    const Quad plane_write = make_quad({rgba8(0, 0, 0), rgba8(0, 0, 0), rgba8(0, 0, 0), rgba8(0, 0, 0)}, 0.25f);
    const Quad probe = make_quad({rgba8(0, 0, 255), rgba8(0, 0, 255), rgba8(0, 0, 255), rgba8(0, 0, 255)}, 0.5f);
    RasterState probe_state;
    probe_state.depth_test = true;
    probe_state.depth_write = false;
    probe_state.depth_function = CompareFunc::Greater;
    VulkanDrawState vk_probe;
    vk_probe.depth_test = true;
    vk_probe.depth_write = false;
    vk_probe.depth_function = CompareFunc::Greater;
    // Case 1: plane written to 0.25, probe at 0.5 passes GREATER on both.
    Framebuffer write_software(32, 32);
    write_software.clear(rgba8(0, 0, 0), 0xFFFFu);
    RasterState always_state;
    always_state.depth_test = true;
    always_state.depth_write = true;
    always_state.depth_function = CompareFunc::Always;
    SoftwareRasterizer write_rasterizer(write_software, always_state);
    draw_quad_software(write_rasterizer, plane_write);
    SoftwareRasterizer probe_rasterizer(write_software, probe_state);
    draw_quad_software(probe_rasterizer, probe);
    VulkanDrawState vk_always;
    vk_always.depth_test = true;
    vk_always.depth_write = true;
    vk_always.depth_function = CompareFunc::Always;
    const std::array<VulkanDrawCall, 2> write_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(plane_write.vk), plane_write.indices, vk_always},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(probe.vk), probe.indices, vk_probe}};
    check(backend.render_headless_draws(write_calls, vulkan, error, 0u), error.c_str());
    compare_frames(write_software, vulkan, 1, false, "clear write is visible to later depth tests");
    // Case 2: plane kept at 1.0, probe at 0.5 fails GREATER on both.
    Framebuffer keep_software(32, 32);
    keep_software.clear(rgba8(0, 0, 0), 0xFFFFu);
    SoftwareRasterizer keep_rasterizer(keep_software, probe_state);
    draw_quad_software(keep_rasterizer, probe);
    const std::array<VulkanDrawCall, 1> keep_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(probe.vk), probe.indices, vk_probe}};
    check(backend.render_headless_draws(keep_calls, vulkan, error, 0u), error.c_str());
    compare_frames(keep_software, vulkan, 1, false, "clear keep leaves the plane untouched");

    Framebuffer blend_software(32, 32);
    blend_software.clear(rgba8(0, 0, 0));
    const std::size_t before_blend = backend.pipeline_count();
    RasterState blend_state;
    blend_state.blend.enabled = true;
    blend_state.blend.source = BlendFactor::SrcAlpha;
    blend_state.blend.destination = BlendFactor::OneMinusSrcAlpha;
    SoftwareRasterizer blend_rasterizer(blend_software, blend_state);
    const std::array<std::array<float, 2>, 3> blend_positions{{{-32.0f, -32.0f}, {96.0f, -32.0f}, {-32.0f, 96.0f}}};
    std::array<ScreenVertex, 3> blend_sw;
    std::array<VulkanDrawVertex, 3> blend_vk;
    for (std::size_t i = 0; i < 3; ++i) {
        blend_sw[i] = {};
        blend_sw[i].x = blend_positions[i][0];
        blend_sw[i].y = blend_positions[i][1];
        blend_sw[i].z = 0.5f;
        blend_sw[i].inv_w = 1.0f;
        blend_sw[i].color = {1.0f, 0.0f, 0.0f, 128.0f / 255.0f};
        blend_vk[i] = {blend_positions[i][0], blend_positions[i][1], 0.5f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                       128.0f / 255.0f};
    }
    blend_rasterizer.draw_triangle(blend_sw[0], blend_sw[1], blend_sw[2]);
    VulkanDrawState vk_blend;
    vk_blend.blend = blend_state.blend;
    const std::array<std::uint16_t, 3> blend_indices{0, 1, 2};
    check(backend.render_headless_draw(blend_vk, blend_indices, vk_blend, vulkan, error, 0u), error.c_str());
    compare_frames(blend_software, vulkan, 1, true, "source-alpha blend matches software");
    check(backend.pipeline_count() == before_blend + 1u, "blend state creates one additional pipeline");

    // Both-FIX complement parity: FIXA + FIXB summing to white per channel
    // must behave as FIX + INVERSE_CONSTANT, not source-wins. Red before the
    // fix (Vulkan double-applies FIXA), green after.
    const std::array<std::array<float, 2>, 3> pair_base_positions{{{-5.0f, -5.0f}, {70.0f, -5.0f}, {-5.0f, 70.0f}}};
    std::array<ScreenVertex, 3> pair_base_sw;
    std::array<VulkanDrawVertex, 3> pair_base_vk;
    for (std::size_t i = 0; i < 3; ++i) {
        pair_base_sw[i] = {};
        pair_base_sw[i].x = pair_base_positions[i][0];
        pair_base_sw[i].y = pair_base_positions[i][1];
        pair_base_sw[i].z = 0.5f;
        pair_base_sw[i].inv_w = 1.0f;
        pair_base_sw[i].color = {0.0f, 0.0f, 128.0f / 255.0f, 1.0f};
        pair_base_vk[i] = {pair_base_positions[i][0], pair_base_positions[i][1], 0.5f, 0.0f, 0.0f, 0.0f, 0.0f,
                           128.0f / 255.0f, 1.0f};
    }
    // Single triangle (no shared-edge double coverage): covers the frame.
    const std::array<std::array<float, 2>, 3> pair_positions{{{-5.0f, -5.0f}, {70.0f, -5.0f}, {-5.0f, 70.0f}}};
    std::array<ScreenVertex, 3> pair_sw;
    std::array<VulkanDrawVertex, 3> pair_vk;
    for (std::size_t i = 0; i < 3; ++i) {
        pair_sw[i] = {};
        pair_sw[i].x = pair_positions[i][0];
        pair_sw[i].y = pair_positions[i][1];
        pair_sw[i].z = 0.5f;
        pair_sw[i].inv_w = 1.0f;
        pair_sw[i].color = {1.0f, 128.0f / 255.0f, 0.0f, 1.0f};
        pair_vk[i] = {pair_positions[i][0], pair_positions[i][1], 0.5f, 0.0f, 0.0f, 1.0f, 128.0f / 255.0f, 0.0f,
                      1.0f};
    }
    const std::array<std::uint16_t, 3> pair_indices{0, 1, 2};
    Framebuffer pair_software(32, 32);
    pair_software.clear(rgba8(0, 0, 0));
    RasterState pair_base_state;
    SoftwareRasterizer pair_base_rasterizer(pair_software, pair_base_state);
    pair_base_rasterizer.draw_triangle(pair_base_sw[0], pair_base_sw[1], pair_base_sw[2]);
    RasterState pair_state;
    pair_state.blend.enabled = true;
    pair_state.blend.source = BlendFactor::Fix;
    pair_state.blend.destination = BlendFactor::Fix;
    pair_state.blend.fixed_source = 0x404040u;
    pair_state.blend.fixed_destination = 0xBFBFBFu;
    SoftwareRasterizer pair_rasterizer(pair_software, pair_state);
    pair_rasterizer.draw_triangle(pair_sw[0], pair_sw[1], pair_sw[2]);
    VulkanDrawState vk_pair_base;
    VulkanDrawState vk_pair;
    vk_pair.blend.enabled = true;
    vk_pair.blend.source = BlendFactor::Fix;
    vk_pair.blend.destination = BlendFactor::Fix;
    vk_pair.blend.fixed_source = 0x404040u;
    vk_pair.blend.fixed_destination = 0xBFBFBFu;
    vk_pair.write_mask = 0u;
    const std::array<VulkanDrawCall, 2> pair_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(pair_base_vk), pair_indices, vk_pair_base},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(pair_vk), pair_indices, vk_pair}};
    check(backend.render_headless_draws(pair_calls, vulkan, error, 0u), error.c_str());
    compare_frames(pair_software, vulkan, 1, false, "both-FIX white pair matches software");

    // PSP modulate (other-color): white over red stays red on both
    // backends. Pre-fix this decoded to (Zero, One) in software and
    // (One, One) in Vulkan, so parity failed.
    const Quad red_base = make_quad({rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0)});
    Framebuffer modulate_software(32, 32);
    modulate_software.clear(rgba8(0, 0, 0));
    RasterState red_state;
    SoftwareRasterizer red_rasterizer(modulate_software, red_state);
    draw_quad_software(red_rasterizer, red_base);
    RasterState modulate_state;
    modulate_state.blend.enabled = true;
    modulate_state.blend.source = BlendFactor::DstColor;
    modulate_state.blend.destination = BlendFactor::OneMinusSrcColor;
    SoftwareRasterizer modulate_rasterizer(modulate_software, modulate_state);
    std::array<ScreenVertex, 3> modulate_sw;
    std::array<VulkanDrawVertex, 3> modulate_vk;
    for (std::size_t i = 0; i < 3; ++i) {
        modulate_sw[i] = {};
        modulate_sw[i].x = blend_positions[i][0];
        modulate_sw[i].y = blend_positions[i][1];
        modulate_sw[i].z = 0.5f;
        modulate_sw[i].inv_w = 1.0f;
        modulate_sw[i].color = {1.0f, 1.0f, 1.0f, 1.0f};
        modulate_vk[i] = {blend_positions[i][0], blend_positions[i][1], 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
                          1.0f};
    }
    modulate_rasterizer.draw_triangle(modulate_sw[0], modulate_sw[1], modulate_sw[2]);
    VulkanDrawState vk_red;
    VulkanDrawState vk_modulate;
    vk_modulate.blend = modulate_state.blend;
    const std::array<VulkanDrawCall, 2> modulate_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(red_base.vk), red_base.indices, vk_red},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(modulate_vk), blend_indices, vk_modulate}};
    check(backend.render_headless_draws(modulate_calls, vulkan, error, 0u), error.c_str());
    compare_frames(modulate_software, vulkan, 1, false, "other-color modulate matches software");
    check(modulate_software.color_at(0, 0) == rgba8(255, 0, 0), "modulate keeps the red base");

    // FIX factors read the latched BLENDFIXED registers via blend constants.
    Framebuffer fix_software(32, 32);
    fix_software.clear(rgba8(255, 0, 0));
    RasterState fix_state;
    fix_state.blend.enabled = true;
    fix_state.blend.source = BlendFactor::Fix;
    fix_state.blend.destination = BlendFactor::Zero;
    fix_state.blend.fixed_source = 0x0000FF00u;
    SoftwareRasterizer fix_rasterizer(fix_software, fix_state);
    fix_rasterizer.draw_triangle(modulate_sw[0], modulate_sw[1], modulate_sw[2]);
    VulkanDrawState vk_fix;
    vk_fix.blend = fix_state.blend;
    check(backend.render_headless_draw(modulate_vk, blend_indices, vk_fix, vulkan, error, 0u), error.c_str());
    compare_frames(fix_software, vulkan, 1, false, "FIX blend matches software");

    // Write mask: an alpha-only clear sprite over red must preserve red.
    // Pre-fix both backends ignored the mask and wrote black.
    Framebuffer mask_software(32, 32);
    mask_software.clear(rgba8(255, 0, 0));
    RasterState mask_state;
    mask_state.blend.enabled = false;
    mask_state.write_mask = 0x00FFFFFFu;
    SoftwareRasterizer mask_rasterizer(mask_software, mask_state);
    std::array<ScreenVertex, 3> mask_sw;
    for (std::size_t i = 0; i < 3; ++i) {
        mask_sw[i] = {};
        mask_sw[i].x = blend_positions[i][0];
        mask_sw[i].y = blend_positions[i][1];
        mask_sw[i].z = 0.5f;
        mask_sw[i].inv_w = 1.0f;
        mask_sw[i].color = {0.0f, 0.0f, 0.0f, 0.0f};
    }
    mask_rasterizer.draw_triangle(mask_sw[0], mask_sw[1], mask_sw[2]);
    std::array<VulkanDrawVertex, 3> mask_vk;
    for (std::size_t i = 0; i < 3; ++i)
        mask_vk[i] = {blend_positions[i][0], blend_positions[i][1], 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    VulkanDrawState vk_mask;
    vk_mask.write_mask = 0x00FFFFFFu;
    const std::array<VulkanDrawCall, 2> mask_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(red_base.vk), red_base.indices, vk_red},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(mask_vk), blend_indices, vk_mask}};
    check(backend.render_headless_draws(mask_calls, vulkan, error, 0u), error.c_str());
    compare_frames(mask_software, vulkan, 1, false, "write mask preserves masked channels");

    // Alpha test: fragments below the reference are discarded on both
    // backends. Pre-fix the Vulkan path had no discard and kept everything.
    // Both targets clear black so a correct discard matches exactly.
    Framebuffer atest_software(32, 32);
    atest_software.clear(rgba8(0, 0, 0));
    RasterState atest_state;
    atest_state.alpha_test = true;
    atest_state.alpha_function = CompareFunc::Greater;
    atest_state.alpha_reference = 200;
    SoftwareRasterizer atest_rasterizer(atest_software, atest_state);
    std::array<ScreenVertex, 3> atest_sw;
    for (std::size_t i = 0; i < 3; ++i) {
        atest_sw[i] = {};
        atest_sw[i].x = blend_positions[i][0];
        atest_sw[i].y = blend_positions[i][1];
        atest_sw[i].z = 0.5f;
        atest_sw[i].inv_w = 1.0f;
        atest_sw[i].color = {0.0f, 1.0f, 0.0f, 128.0f / 255.0f};
    }
    atest_rasterizer.draw_triangle(atest_sw[0], atest_sw[1], atest_sw[2]);
    std::array<VulkanDrawVertex, 3> atest_vk;
    for (std::size_t i = 0; i < 3; ++i)
        atest_vk[i] = {blend_positions[i][0], blend_positions[i][1], 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 128.0f / 255.0f};
    VulkanDrawState vk_atest;
    vk_atest.alpha_test = true;
    vk_atest.alpha_function = CompareFunc::Greater;
    vk_atest.alpha_reference = 200;
    check(backend.render_headless_draw(atest_vk, blend_indices, vk_atest, vulkan, error, 0u), error.c_str());
    compare_frames(atest_software, vulkan, 1, false, "alpha test discards below reference");

    // Additive overflow: software clamps at white; the Vulkan path must
    // agree (UNORM blend clamps) rather than wrap mod 256. Two white-over-
    // white passes separate clamp (255) from wrap (253) beyond tolerance.
    // A solid-white quad avoids rasterizer interpolation differences.
    const Quad white = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                  rgba8(255, 255, 255)});
    Framebuffer clamp_software(32, 32);
    clamp_software.clear(rgba8(0, 0, 0));
    RasterState clamp_state;
    clamp_state.blend.enabled = true;
    clamp_state.blend.source = BlendFactor::One;
    clamp_state.blend.destination = BlendFactor::One;
    clamp_state.blend.equation = BlendEquation::Add;
    SoftwareRasterizer clamp_rasterizer(clamp_software, clamp_state);
    draw_quad_software(clamp_rasterizer, white);
    draw_quad_software(clamp_rasterizer, white);
    VulkanDrawState vk_clamp;
    vk_clamp.blend = clamp_state.blend;
    const std::array<VulkanDrawCall, 2> clamp_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(white.vk), white.indices, vk_clamp},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(white.vk), white.indices, vk_clamp}};
    check(backend.render_headless_draws(clamp_calls, vulkan, error, 0u), error.c_str());
    compare_frames(clamp_software, vulkan, 1, false, "additive overflow clamps on both");

    // Fog: full fog turns white into the fog color on both backends.
    Quad fog_quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                               rgba8(255, 255, 255)});
    for (auto &vertex : fog_quad.sw) vertex.fog = 0.0f;
    for (auto &vertex : fog_quad.vk) vertex.fog = 0.0f;
    Framebuffer fog_software(32, 32);
    fog_software.clear(rgba8(0, 0, 0));
    RasterState fog_state;
    fog_state.fog_enabled = true;
    fog_state.fog_color = 0x0000FF00u;
    SoftwareRasterizer fog_rasterizer(fog_software, fog_state);
    draw_quad_software(fog_rasterizer, fog_quad);
    VulkanDrawState vk_fog;
    vk_fog.fog_enabled = true;
    vk_fog.fog_color = 0x0000FF00u;
    check(backend.render_headless_draw(fog_quad.vk, fog_quad.indices, vk_fog, vulkan, error, 0u), error.c_str());
    compare_frames(fog_software, vulkan, 1, false, "fog mixes toward the fog color on both");

    // Repeat wrap with out-of-range UVs: the game's world quads carry
    // scaled/offset UVs like (1.75, -4.13). Software repeats; pre-fix the
    // Vulkan sampler clamped (clamp flags were fed to repeat-semantics
    // params), stretching edge texels instead of tiling.
    std::vector<std::uint8_t> wrap_data(2u * 2u * 4u);
    const std::uint32_t wrap_colors[4] = {rgba8(255, 0, 0), rgba8(0, 255, 0), rgba8(0, 0, 255),
                                          rgba8(255, 255, 255)};
    for (int i = 0; i < 4; ++i) {
        wrap_data[static_cast<std::size_t>(i) * 4u] = static_cast<std::uint8_t>(wrap_colors[i] & 0xFFu);
        wrap_data[static_cast<std::size_t>(i) * 4u + 1u] =
            static_cast<std::uint8_t>((wrap_colors[i] >> 8u) & 0xFFu);
        wrap_data[static_cast<std::size_t>(i) * 4u + 2u] =
            static_cast<std::uint8_t>((wrap_colors[i] >> 16u) & 0xFFu);
        wrap_data[static_cast<std::size_t>(i) * 4u + 3u] = 255u;
    }
    TextureState wrap_state;
    wrap_state.format = TextureFormat::Rgba8888;
    wrap_state.width = 2;
    wrap_state.height = 2;
    wrap_state.buffer_width = 2;
    wrap_state.clamp_u = false;
    wrap_state.clamp_v = false;
    wrap_state.linear = false;
    const Texture wrap_texture(wrap_state, wrap_data);
    TextureImage wrap_image;
    wrap_image.width = 2;
    wrap_image.height = 2;
    wrap_texture.decode(wrap_image.pixels);
    Quad wrap_quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                rgba8(255, 255, 255)});
    const std::array<std::array<float, 2>, 4> wrap_uvs{{{0.0f, 0.0f}, {2.0f, 0.0f}, {2.0f, 2.0f}, {0.0f, 2.0f}}};
    for (std::size_t i = 0; i < 4; ++i) {
        wrap_quad.sw[i].texcoord = {wrap_uvs[i][0], wrap_uvs[i][1], 1.0f};
        wrap_quad.vk[i].u = wrap_uvs[i][0];
        wrap_quad.vk[i].v = wrap_uvs[i][1];
    }
    Framebuffer wrap_software(32, 32);
    wrap_software.clear(rgba8(0, 0, 0));
    RasterState wrap_raster_state;
    wrap_raster_state.texture_enabled = true;
    wrap_raster_state.texture = &wrap_texture;
    SoftwareRasterizer wrap_rasterizer(wrap_software, wrap_raster_state);
    draw_quad_software(wrap_rasterizer, wrap_quad);
    VulkanDrawState vk_wrap;
    vk_wrap.texture = &wrap_image;
    vk_wrap.texture_key = wrap_texture.content_key();
    vk_wrap.clamp_u = false;
    vk_wrap.clamp_v = false;
    check(backend.render_headless_draw(wrap_quad.vk, wrap_quad.indices, vk_wrap, vulkan, error, 0u), error.c_str());
    compare_frames(wrap_software, vulkan, 1, false, "repeat wrap tiles out-of-range UVs on both");

    // Texture functions: each combine must agree between backends.
    // Pre-fix both paths only modulated, so decal/replace/blend/add
    // mismatched any game using them (observed red below via software).
    std::vector<std::uint8_t> func_data(2u * 2u * 4u);
    const std::uint32_t func_colors[4] = {rgba8(64, 128, 192, 128), rgba8(255, 0, 0), rgba8(0, 255, 0),
                                          rgba8(0, 0, 255)};
    for (int i = 0; i < 4; ++i) {
        func_data[static_cast<std::size_t>(i) * 4u] = static_cast<std::uint8_t>(func_colors[i] & 0xFFu);
        func_data[static_cast<std::size_t>(i) * 4u + 1u] =
            static_cast<std::uint8_t>((func_colors[i] >> 8u) & 0xFFu);
        func_data[static_cast<std::size_t>(i) * 4u + 2u] =
            static_cast<std::uint8_t>((func_colors[i] >> 16u) & 0xFFu);
        func_data[static_cast<std::size_t>(i) * 4u + 3u] =
            static_cast<std::uint8_t>((func_colors[i] >> 24u) & 0xFFu);
    }
    TextureState func_state;
    func_state.format = TextureFormat::Rgba8888;
    func_state.width = 2;
    func_state.height = 2;
    func_state.buffer_width = 2;
    func_state.clamp_u = true;
    func_state.clamp_v = true;
    const Texture func_texture(func_state, func_data);
    TextureImage func_image;
    func_image.width = 2;
    func_image.height = 2;
    func_texture.decode(func_image.pixels);
    struct FuncCase {
        TextureFunc func;
        bool rgba;
        bool dbl;
        std::uint32_t env;
        const char *name;
    };
    const std::array<FuncCase, 5> func_cases{{{TextureFunc::Decal, true, false, 0u, "decal RGBA"},
                                             {TextureFunc::Blend, false, false, 0x0000FFu, "blend env"},
                                             {TextureFunc::Replace, true, false, 0u, "replace RGBA"},
                                             {TextureFunc::Add, false, false, 0u, "add"},
                                             {TextureFunc::Modulate, false, true, 0u, "modulate double"}}};
    for (const FuncCase &func_case : func_cases) {
        const Quad quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                     rgba8(255, 255, 255)},
                                    0.5f, true);
        Framebuffer func_software(32, 32);
        func_software.clear(rgba8(0, 0, 0));
        RasterState func_raster_state;
        func_raster_state.texture_enabled = true;
        func_raster_state.texture = &func_texture;
        func_raster_state.tex_func = func_case.func;
        func_raster_state.tex_rgba = func_case.rgba;
        func_raster_state.color_double = func_case.dbl;
        func_raster_state.tex_env = func_case.env;
        SoftwareRasterizer func_rasterizer(func_software, func_raster_state);
        draw_quad_software(func_rasterizer, quad);
        VulkanDrawState vk_func;
        vk_func.texture = &func_image;
        vk_func.texture_key = func_texture.content_key();
        vk_func.tex_func = func_case.func;
        vk_func.tex_rgba = func_case.rgba;
        vk_func.color_double = func_case.dbl;
        vk_func.tex_env = func_case.env;
        check(backend.render_headless_draw(quad.vk, quad.indices, vk_func, vulkan, error, 0u), error.c_str());
        compare_frames(func_software, vulkan, 1, false, func_case.name);
    }

    // Render targets: two FBP addresses land on different surfaces; the
    // display buffer selects which one presents. Pre-target-routing the
    // backend mashed everything into one image.
    TargetSet router(32, 32);
    const Quad red_quad = make_quad({rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0)});
    const Quad green_quad =
        make_quad({rgba8(0, 255, 0), rgba8(0, 255, 0), rgba8(0, 255, 0), rgba8(0, 255, 0)});
    {
        TargetEntry &entry = router.draw_to(0x100u, TextureFormat::Rgba8888);
        SoftwareRasterizer rasterizer(entry.surface);
        draw_quad_software(rasterizer, red_quad);
        entry.dirty = true;
    }
    {
        TargetEntry &entry = router.draw_to(0x200u, TextureFormat::Rgba8888);
        SoftwareRasterizer rasterizer(entry.surface);
        draw_quad_software(rasterizer, green_quad);
        entry.dirty = true;
    }
    VulkanDrawState vk_tred;
    vk_tred.draw_buffer = 0x100u;
    VulkanDrawState vk_tgreen;
    vk_tgreen.draw_buffer = 0x200u;
    const std::array<VulkanDrawCall, 2> target_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(red_quad.vk), red_quad.indices, vk_tred},
        VulkanDrawCall{std::span<const VulkanDrawVertex>(green_quad.vk), green_quad.indices, vk_tgreen}};
    check(backend.render_headless_draws(target_calls, vulkan, error, 0x100u), error.c_str());
    check(router.display(0x100u) != nullptr, "software display target exists");
    compare_frames(*router.display(0x100u), vulkan, 1, false, "display target holds only its draws");

    // 16-bit display target: (128, 128, 128) quantizes to
    // (131, 129, 131) on both backends.
    const Quad gray_quad =
        make_quad({rgba8(128, 128, 128), rgba8(128, 128, 128), rgba8(128, 128, 128), rgba8(128, 128, 128)});
    TargetSet fmt_router(32, 32);
    TargetEntry &fmt_entry = fmt_router.draw_to(0x300u, TextureFormat::Rgb565);
    RasterState fmt_state;
    fmt_state.fb_format = TextureFormat::Rgb565;
    SoftwareRasterizer fmt_rasterizer(fmt_entry.surface, fmt_state);
    draw_quad_software(fmt_rasterizer, gray_quad);
    fmt_entry.dirty = true;
    VulkanDrawState vk_fmt;
    vk_fmt.draw_buffer = 0x300u;
    vk_fmt.fb_format = TextureFormat::Rgb565;
    const std::array<VulkanDrawCall, 1> fmt_calls{
        VulkanDrawCall{std::span<const VulkanDrawVertex>(gray_quad.vk), gray_quad.indices, vk_fmt}};
    check(backend.render_headless_draws(fmt_calls, vulkan, error, 0x300u), error.c_str());
    compare_frames(*fmt_router.display(0x300u), vulkan, 1, false, "5650 display target quantizes on both");

    // Lines: a horizontal white line must match between backends.
    // Pre-fix the Vulkan path had triangle-only topology and the state
    // machine dropped lines entirely.
    Framebuffer line_software(32, 32);
    line_software.clear(rgba8(0, 0, 0));
    RasterState line_state;
    SoftwareRasterizer line_rasterizer(line_software, line_state);
    ScreenVertex line_a = {};
    line_a.x = 0.0f;
    // y=16.25 sits unambiguously inside row 16 (an exact integer y lies
    // on a row boundary, where GPU tie-breaking differs by a row).
    line_a.y = 16.25f;
    line_a.z = 0.5f;
    line_a.inv_w = 1.0f;
    line_a.color = {1.0f, 1.0f, 1.0f, 1.0f};
    ScreenVertex line_b = line_a;
    line_b.x = 32.0f;
    line_rasterizer.draw_line(line_a, line_b);
    const std::array<VulkanDrawVertex, 2> line_vk{{{0.0f, 16.25f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
                                                  {32.0f, 16.25f, 0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f}}};
    const std::array<std::uint16_t, 2> line_indices{0, 1};
    VulkanDrawState vk_line;
    vk_line.primitive = Primitive::Lines;
    check(backend.render_headless_draw(line_vk, line_indices, vk_line, vulkan, error, 0u), error.c_str());
    compare_frames(line_software, vulkan, 1, false, "horizontal line matches software");

    // Stencil: Equal against a cleared plane rejects; Always passes and
    // the zpass REPLACE writes the reference. Pre-enforcement everything
    // drew unconditionally.
    Framebuffer stencil_software(32, 32);
    stencil_software.clear(rgba8(0, 0, 0), 0xFFFFu, 0u);
    RasterState stencil_state;
    stencil_state.scissor_x0 = 0;
    stencil_state.scissor_y0 = 0;
    stencil_state.scissor_x1 = 32;
    stencil_state.scissor_y1 = 32;
    stencil_state.stencil.test = true;
    stencil_state.stencil.function = CompareFunc::Equal;
    stencil_state.stencil.reference = 1;
    stencil_state.stencil.mask = 0xFFu;
    stencil_state.stencil.zpass = StencilOp::Replace;
    SoftwareRasterizer stencil_rasterizer(stencil_software, stencil_state);
    const Quad stencil_quad =
        make_quad({rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0), rgba8(255, 0, 0)});
    draw_quad_software(stencil_rasterizer, stencil_quad);
    VulkanDrawState vk_stencil;
    vk_stencil.stencil.test = true;
    vk_stencil.stencil.function = CompareFunc::Equal;
    vk_stencil.stencil.reference = 1;
    vk_stencil.stencil.mask = 0xFFu;
    vk_stencil.stencil.zpass = StencilOp::Replace;
    check(backend.render_headless_draw(stencil_quad.vk, stencil_quad.indices, vk_stencil, vulkan, error, 0u),
          error.c_str());
    compare_frames(stencil_software, vulkan, 1, false, "stencil reject matches software");
    check(stencil_software.color_at(0, 0) == rgba8(0, 0, 0), "equal-against-zero rejects the quad");
    stencil_state.stencil.function = CompareFunc::Always;
    stencil_rasterizer.state() = stencil_state;
    draw_quad_software(stencil_rasterizer, stencil_quad);
    vk_stencil.stencil.function = CompareFunc::Always;
    check(backend.render_headless_draw(stencil_quad.vk, stencil_quad.indices, vk_stencil, vulkan, error, 0u),
          error.c_str());
    compare_frames(stencil_software, vulkan, 1, false, "stencil replace matches software");
    check(stencil_software.color_at(0, 0) == rgba8(255, 0, 0) &&
              stencil_software.stencil_at(0, 0) == 1u,
          "always passes and writes the reference");

    // Ground-strip combo (battle draws 73-class): textured triangle strip
    // (expanded to a triangle list at emit, primitive stays strips) with
    // stencil Always/REPLACE, depth Greater, and SrcAlpha blend. The
    // terrain renders on host software but was missing on device.
    {
        TextureState strip_state;
        strip_state.format = TextureFormat::Rgba8888;
        strip_state.width = 8;
        strip_state.height = 8;
        strip_state.buffer_width = 8;
        std::vector<std::uint8_t> strip_pixels(8u * 8u * 4u);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                const bool light = ((x + y) & 1) == 0;
                const std::size_t o = static_cast<std::size_t>(y * 8 + x) * 4u;
                strip_pixels[o] = light ? 139u : 90u;
                strip_pixels[o + 1u] = light ? 90u : 60u;
                strip_pixels[o + 2u] = light ? 43u : 30u;
                strip_pixels[o + 3u] = 255u;
            }
        const Texture strip_texture(strip_state, strip_pixels, {});
        TextureImage strip_image;
        strip_image.width = 8;
        strip_image.height = 8;
        strip_texture.decode(strip_image.pixels);

        // Two adjacent quads as an expanded strip (v0v1v2, v2v1v3, ...).
        const std::array<ScreenVertex, 8> strip_sw = {[&] {
            std::array<ScreenVertex, 8> out{};
            const float xs[8] = {0.0f, 16.0f, 0.0f, 16.0f, 16.0f, 32.0f, 16.0f, 32.0f};
            const float ys[8] = {0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 0.0f, 32.0f, 32.0f};
            for (int i = 0; i < 8; ++i) {
                out[i].x = xs[i];
                out[i].y = ys[i];
                // Depth comfortably above the cleared plane on both
                // backends (software compares 16-bit ints, Vulkan NDC).
                out[i].z = 30000.0f;
                out[i].inv_w = 1.0f;
                out[i].texcoord = {xs[i] / 32.0f, ys[i] / 32.0f, 1.0f};
                out[i].color = {0.867f, 0.867f, 0.667f, 1.0f};
            }
            return out;
        }()};
        const std::array<std::uint16_t, 12> strip_indices{0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
        Framebuffer strip_software(32, 32);
        strip_software.clear(rgba8(0, 0, 0), 0x0000u);
        RasterState strip_raster_state;
        strip_raster_state.scissor_x1 = 32;
        strip_raster_state.scissor_y1 = 32;
        // Mirror the game's clear discipline first: an ALWAYS+write draw
        // at z=0 (the headless plane starts at 1.0, software at 0 here).
        strip_raster_state.depth_test = true;
        strip_raster_state.depth_write = true;
        strip_raster_state.depth_function = CompareFunc::Always;
        SoftwareRasterizer strip_rasterizer(strip_software, strip_raster_state);
        {
            ScreenVertex a = strip_sw[0];
            ScreenVertex b = strip_sw[1];
            ScreenVertex c = strip_sw[2];
            a.z = b.z = c.z = 0.0f;
            a.color = b.color = c.color = {0.0f, 0.0f, 0.0f, 0.0f};
            strip_rasterizer.draw_triangle(a, b, c);
            ScreenVertex d = strip_sw[2];
            ScreenVertex e = strip_sw[1];
            ScreenVertex f = strip_sw[3];
            d.z = e.z = f.z = 0.0f;
            d.color = e.color = f.color = {0.0f, 0.0f, 0.0f, 0.0f};
            strip_rasterizer.draw_triangle(d, e, f);
        }
        strip_raster_state.depth_function = CompareFunc::Greater;
        strip_raster_state.stencil.test = true;
        strip_raster_state.stencil.function = CompareFunc::Always;
        strip_raster_state.stencil.reference = 2;
        strip_raster_state.stencil.zpass = StencilOp::Replace;
        strip_raster_state.blend.enabled = true;
        strip_raster_state.blend.source = BlendFactor::SrcAlpha;
        strip_raster_state.blend.destination = BlendFactor::OneMinusSrcAlpha;
        strip_raster_state.texture_enabled = true;
        strip_raster_state.texture = &strip_texture;
        strip_rasterizer.state() = strip_raster_state;
        for (std::size_t i = 0; i + 2u < strip_indices.size(); i += 3u)
            strip_rasterizer.draw_triangle(strip_sw[strip_indices[i]], strip_sw[strip_indices[i + 1u]],
                                           strip_sw[strip_indices[i + 2u]]);
        const std::array<VulkanDrawVertex, 8> strip_vk = {[&] {
            std::array<VulkanDrawVertex, 8> out{};
            for (int i = 0; i < 8; ++i)
                out[i] = {strip_sw[i].x, strip_sw[i].y, strip_sw[i].z, strip_sw[i].texcoord[0],
                          strip_sw[i].texcoord[1], strip_sw[i].color[0], strip_sw[i].color[1],
                          strip_sw[i].color[2], strip_sw[i].color[3]};
            return out;
        }()};
        VulkanDrawState vk_strip;
        vk_strip.primitive = Primitive::TriangleStrip;
        vk_strip.texture = &strip_image;
        vk_strip.texture_key = strip_texture.content_key();
        vk_strip.depth_test = true;
        vk_strip.depth_write = true;
        vk_strip.depth_function = CompareFunc::Greater;
        vk_strip.stencil.test = true;
        vk_strip.stencil.function = CompareFunc::Always;
        vk_strip.stencil.reference = 2;
        vk_strip.stencil.zpass = StencilOp::Replace;
        vk_strip.blend.enabled = true;
        vk_strip.blend.source = BlendFactor::SrcAlpha;
        vk_strip.blend.destination = BlendFactor::OneMinusSrcAlpha;
        // Headless plane starts at depth 1.0: clear it with an ALWAYS
        // write at z=0 first, mirroring the game's vertex-driven clears.
        const std::array<VulkanDrawVertex, 4> strip_clear_vk{{
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {32.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {32.0f, 32.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
            {0.0f, 32.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        }};
        const std::array<std::uint16_t, 6> strip_clear_indices{0, 1, 2, 0, 2, 3};
        VulkanDrawState vk_strip_clear;
        vk_strip_clear.depth_test = true;
        vk_strip_clear.depth_write = true;
        vk_strip_clear.depth_function = CompareFunc::Always;
        const std::array<VulkanDrawCall, 2> strip_calls{
            VulkanDrawCall{std::span<const VulkanDrawVertex>(strip_clear_vk), strip_clear_indices, vk_strip_clear},
            VulkanDrawCall{std::span<const VulkanDrawVertex>(strip_vk), strip_indices, vk_strip}};
        check(backend.render_headless_draws(strip_calls, vulkan, error, 0u), error.c_str());
        compare_frames(strip_software, vulkan, 1, false, "textured strip with stencil+depth matches software");
        check(strip_software.color_at(4, 4) != rgba8(0, 0, 0), "strip painted the field");
    }

    // Projective UVs: the software rasterizer divides by q as-is (q == 0
    // means 1), so negative q flips the sign of s/t back into range.
    // The fragment shader must mirror that; clamping q with max(q, eps)
    // turns every negative-q texel into garbage (battle terrain class).
    for (const float q : {-2.0f, 0.0f}) {
        std::vector<std::uint8_t> q_data(4u * 4u * 4u);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) {
                const std::size_t o = static_cast<std::size_t>(y * 4 + x) * 4u;
                const bool target = (x == 1 && y == 2);
                q_data[o] = target ? 255u : 0u;
                q_data[o + 1u] = target ? 0u : 0u;
                q_data[o + 2u] = target ? 255u : 0u;
                q_data[o + 3u] = 255u;
            }
        TextureState q_state;
        q_state.format = TextureFormat::Rgba8888;
        q_state.width = 4;
        q_state.height = 4;
        q_state.buffer_width = 4;
        q_state.clamp_u = true;
        q_state.clamp_v = true;
        q_state.linear = false;
        const Texture q_texture(q_state, q_data);
        TextureImage q_image;
        q_image.width = 4;
        q_image.height = 4;
        q_texture.decode(q_image.pixels);
        // Constant (s, t) chosen so s/q = 0.375 (texel x=1) and
        // t/q = 0.625 (texel y=2): the magenta texel. With q = -2 that is
        // s = -0.75, t = -1.25; with q = 0 the software treats q as 1.
        const float qs = (q == 0.0f) ? 0.375f : q * 0.375f;
        const float qt = (q == 0.0f) ? 0.625f : q * 0.625f;
        Quad q_quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                 rgba8(255, 255, 255)});
        for (std::size_t i = 0; i < 4; ++i) {
            q_quad.sw[i].texcoord = {qs, qt, q};
            q_quad.vk[i].u = qs;
            q_quad.vk[i].v = qt;
            q_quad.vk[i].q = q;
        }
        Framebuffer q_software(32, 32);
        q_software.clear(rgba8(0, 0, 0));
        RasterState q_raster_state;
        q_raster_state.texture_enabled = true;
        q_raster_state.texture = &q_texture;
        SoftwareRasterizer q_rasterizer(q_software, q_raster_state);
        draw_quad_software(q_rasterizer, q_quad);
        VulkanDrawState vk_q;
        vk_q.texture = &q_image;
        vk_q.texture_key = q_texture.content_key();
        vk_q.clamp_u = true;
        vk_q.clamp_v = true;
        check(backend.render_headless_draw(q_quad.vk, q_quad.indices, vk_q, vulkan, error, 0u),
              error.c_str());
        check(q_software.color_at(16, 16) == rgba8(255, 0, 255),
              "projective UV fixture targets the magenta texel");
        compare_frames(q_software, vulkan, 1, false,
                       q == 0.0f ? "zero q matches software" : "negative q matches software");
    }

    // Backface culling: guest-CCW triangles with front=CCW survive cull-Back
    // on both backends, and guest-CW triangles with front=CW do too. The
    // mapping previously only created a pipeline here without checking
    // pixels, which is how an inverted front-face mapping hid: cull-Back
    // draws vanished on device while cull-None painted.
    {
        const Quad cull_quad = make_quad({rgba8(200, 100, 40), rgba8(200, 100, 40), rgba8(200, 100, 40),
                                          rgba8(200, 100, 40)});
        const std::array<std::uint16_t, 6> cull_ccw_indices{0, 1, 2, 0, 2, 3};
        const std::array<std::uint16_t, 6> cull_cw_indices{0, 2, 1, 0, 3, 2};
        for (const bool clockwise : {false, true}) {
            const std::array<std::uint16_t, 6> &indices = clockwise ? cull_cw_indices : cull_ccw_indices;
            Framebuffer cull_software(32, 32);
            cull_software.clear(rgba8(0, 0, 0));
            SoftwareRasterizer cull_rasterizer(cull_software);
            cull_rasterizer.draw_triangle(cull_quad.sw[indices[0]], cull_quad.sw[indices[1]],
                                          cull_quad.sw[indices[2]]);
            cull_rasterizer.draw_triangle(cull_quad.sw[indices[3]], cull_quad.sw[indices[4]],
                                          cull_quad.sw[indices[5]]);
            VulkanDrawState vk_cull;
            vk_cull.cull = CullMode::Back;
            vk_cull.front_face = clockwise ? FrontFace::Clockwise : FrontFace::CounterClockwise;
            check(backend.render_headless_draw(cull_quad.vk, indices, vk_cull, vulkan, error, 0u),
                  error.c_str());
            check(cull_software.color_at(16, 16) == rgba8(200, 100, 40),
                  "cull fixture paints its quad in software");
            compare_frames(cull_software, vulkan, 1, false,
                           clockwise ? "cull-Back keeps CW-front triangles"
                                     : "cull-Back keeps CCW-front triangles");
        }
    }

    Framebuffer scissor_software(32, 32);
    scissor_software.clear(rgba8(0, 0, 0));
    RasterState scissor_state;
    scissor_state.scissor_x0 = 8;
    scissor_state.scissor_y0 = 8;
    scissor_state.scissor_x1 = 24;
    scissor_state.scissor_y1 = 24;
    SoftwareRasterizer scissor_rasterizer(scissor_software, scissor_state);
    draw_quad_software(scissor_rasterizer, solid);
    VulkanDrawState vk_scissor;
    vk_scissor.scissor_x0 = 8;
    vk_scissor.scissor_y0 = 8;
    vk_scissor.scissor_x1 = 24;
    vk_scissor.scissor_y1 = 24;
    check(backend.render_headless_draw(solid.vk, solid.indices, vk_scissor, vulkan, error, 0u), error.c_str());
    compare_frames(scissor_software, vulkan, 1, false, "scissor rectangle matches software");

    const std::vector<std::uint8_t> texture_data{0u, 1u, 2u, 3u};
    std::vector<std::uint8_t> palette(4u * 2u);
    const std::uint16_t palette_colors[4] = {0x001Fu, 0x07E0u, 0xF800u, 0xFFFFu};
    for (int i = 0; i < 4; ++i) {
        palette[static_cast<std::size_t>(i * 2)] = static_cast<std::uint8_t>(palette_colors[i] & 0xFFu);
        palette[static_cast<std::size_t>(i * 2 + 1)] = static_cast<std::uint8_t>(palette_colors[i] >> 8u);
    }
    TextureState texture_state;
    texture_state.format = TextureFormat::Clut8;
    texture_state.width = 2;
    texture_state.height = 2;
    texture_state.buffer_width = 2;
    texture_state.clamp_u = true;
    texture_state.clamp_v = true;
    texture_state.clut_format = 0xFFu << 8u;
    const Texture texture(texture_state, texture_data, palette);
    TextureImage texture_image;
    texture_image.width = 2;
    texture_image.height = 2;
    texture.decode(texture_image.pixels);

    Framebuffer texture_software(32, 32);
    texture_software.clear(rgba8(0, 0, 0));
    RasterState texture_raster_state;
    texture_raster_state.texture_enabled = true;
    texture_raster_state.texture = &texture;
    SoftwareRasterizer texture_rasterizer(texture_software, texture_raster_state);
    const Quad textured = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                     rgba8(255, 255, 255)},
                                    0.5f, true);
    draw_quad_software(texture_rasterizer, textured);
    VulkanDrawState vk_texture;
    vk_texture.texture = &texture_image;
    const std::uint64_t texture_key = texture.content_key();
    vk_texture.texture_key = texture_key;
    check(backend.render_headless_draw(textured.vk, textured.indices, vk_texture, vulkan, error, 0u), error.c_str());
    compare_frames(texture_software, vulkan, 1, false, "CLUT-decoded texture matches software");
    TextureRangeIndex ranges;
    ranges.track(0x04088000u, 0x1000u, texture_key);
    check(ranges.invalidate_write(0x04088000u, 0x100u, [&backend](std::uint64_t key) {
              backend.invalidate_texture(key);
          }) == 1u,
          "render-to-texture range invalidates the GPU texture");
    std::vector<std::uint8_t> changed_palette = palette;
    std::swap(changed_palette[0], changed_palette[4]);
    std::swap(changed_palette[2], changed_palette[6]);
    const Texture changed_texture(texture_state, texture_data, changed_palette);
    TextureImage changed_image;
    changed_image.width = 2;
    changed_image.height = 2;
    changed_texture.decode(changed_image.pixels);
    texture_software.clear(rgba8(0, 0, 0));
    texture_rasterizer.state().texture = &changed_texture;
    draw_quad_software(texture_rasterizer, textured);
    vk_texture.texture = &changed_image;
    check(backend.render_headless_draw(textured.vk, textured.indices, vk_texture, vulkan, error, 0u), error.c_str());
    compare_frames(texture_software, vulkan, 1, false, "invalidated texture re-uploads and matches software");

    backend.shutdown();

    VulkanBackend validation_backend;
    VulkanBackend::Config validation_config = config;
    validation_config.validation = true;
    if (!validation_backend.initialize(validation_config, error))
        check(error == "validation_layer_unavailable", "missing validation layer is a named error");
    else
        validation_backend.shutdown();
}

#if defined(GUNGNIR_HAVE_SDL3)
void windowed_swapchain() {
    if (SDL_Init(SDL_INIT_VIDEO) == false) {
        std::cout << "SKIP: sdl_video_unavailable: " << SDL_GetError() << "\n";
        return;
    }
    SDL_Window *window = SDL_CreateWindow("gungnir-vulkan-test", 256, 256, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (window == nullptr) {
        std::cout << "SKIP: sdl_window_unavailable: " << SDL_GetError() << "\n";
        SDL_Quit();
        return;
    }

    Uint32 extension_count = 0;
    char const *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    check(extensions != nullptr && extension_count != 0u, "SDL reports Vulkan instance extensions");

    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 256;
    config.height = 256;
    config.headless = false;
    config.instance_extensions.assign(extensions, extensions + extension_count);
    config.create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) return VK_NULL_HANDLE;
        return surface;
    };
    std::string error;
    if (!backend.initialize(config, error)) {
        std::cout << "SKIP: windowed_vulkan_unavailable: " << error << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return;
    }
    check(backend.render_swapchain(error), error.c_str());
    check(backend.recreate_swapchain(320, 240, error), error.c_str());
    check(backend.render_swapchain(error), error.c_str());
    check(backend.recreate_surface(error), error.c_str());
    check(backend.render_swapchain(error), error.c_str());
    backend.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
}
#endif

} // namespace

void many_textures() {
    std::string reason;
    if (!VulkanBackend::available(reason)) {
        std::cout << "SKIP: " << reason << "\n";
        std::exit(77);
    }
    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 32;
    config.height = 32;
    config.headless = true;
    std::string error;
    check(backend.initialize(config, error), error.c_str());

    const Quad quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                 rgba8(255, 255, 255)},
                                0.5f, true);
    std::vector<TextureImage> images;
    images.reserve(96u);
    for (int i = 0; i < 96; ++i) {
        TextureImage image;
        image.width = 2;
        image.height = 2;
        image.pixels.assign(4u, rgba8(static_cast<std::uint32_t>(i * 2), 64, 128));
        images.push_back(std::move(image));
    }
    VulkanFrame frame;
    for (std::size_t i = 0; i < images.size(); ++i) {
        VulkanDrawState state;
        state.texture = &images[i];
        state.texture_key = 0x1000u + i;
        const std::string contract = "the texture descriptor pool holds " +
            std::to_string(images.size()) + " live textures (failed at " + std::to_string(i) + ")";
        check(backend.render_headless_draw(quad.vk, quad.indices, state, frame, error, 0u), contract.c_str());
    }
    backend.shutdown();
}

void bounded_gpu_texture_cache() {
    std::string reason;
    if (!VulkanBackend::available(reason)) {
        std::cout << "SKIP: " << reason << "\n";
        std::exit(77);
    }
    VulkanBackend backend;
    VulkanBackend::Config config;
    config.width = 32;
    config.height = 32;
    config.headless = true;
    config.texture_cache_limit = 8u;
    config.texture_cache_bytes = 1u << 20u;
    std::string error;
    check(backend.initialize(config, error), error);

    const Quad quad = make_quad({rgba8(255, 255, 255), rgba8(255, 255, 255), rgba8(255, 255, 255),
                                 rgba8(255, 255, 255)},
                                0.5f, true);
    std::vector<TextureImage> images;
    images.reserve(32u);
    for (int i = 0; i < 32; ++i) {
        TextureImage image;
        image.width = 2;
        image.height = 2;
        image.pixels.assign(4u, rgba8(static_cast<std::uint32_t>(i * 7), 32, 64));
        images.push_back(std::move(image));
    }
    VulkanFrame frame;
    for (std::size_t i = 0; i < images.size(); ++i) {
        VulkanDrawState state;
        state.texture = &images[i];
        state.texture_key = 0x8000u + i;
        check(backend.render_headless_draw(quad.vk, quad.indices, state, frame, error, 0u), error);
        check(backend.texture_count() <= config.texture_cache_limit,
              "the GPU texture cache keeps at most its configured entry count (frame " + std::to_string(i) +
                  " held " + std::to_string(backend.texture_count()) + ")");
        check(backend.texture_bytes() <= config.texture_cache_bytes,
              "the GPU texture cache keeps at most its configured byte budget");
    }
    backend.shutdown();
}

int main() {
    try {
        shader_validation();
        viewport_fitting();
        scissor_scaling();
        headless_triangle();
        depth_format_tests();
        sampler_sharing_tests();
        parity_tests();
        many_textures();
        bounded_gpu_texture_cache();
#if defined(GUNGNIR_HAVE_SDL3)
        windowed_swapchain();
#endif
        std::cout << "ge_vulkan_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

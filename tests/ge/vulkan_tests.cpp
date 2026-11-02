// T7e contract tests: embedded shaders, headless Vulkan triangle readback and
// windowed swapchain creation/recreation.

#include "ge/vulkan_backend.hpp"
#include "ge/vulkan_shaders.hpp"

#include "ge/software.hpp"
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
    check(backend.render_headless_draw(solid.vk, solid.indices, solid_state, vulkan, error), error.c_str());
    compare_frames(software, vulkan, 1, false, "solid colour quad matches software");
    const std::size_t first_pipeline_count = backend.pipeline_count();
    check(first_pipeline_count == 1u, "one pipeline for one draw state");
    check(backend.render_headless_draw(solid.vk, solid.indices, solid_state, vulkan, error), error.c_str());
    check(backend.pipeline_count() == first_pipeline_count, "repeated state reuses the pipeline");
    VulkanDrawState cull_state;
    cull_state.cull = CullMode::Back;
    cull_state.front_face = FrontFace::Clockwise;
    check(backend.render_headless_draw(solid.vk, solid.indices, cull_state, vulkan, error), error.c_str());
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
    check(backend.render_headless_draws(depth_calls, vulkan, error), error.c_str());
    compare_frames(depth_software, vulkan, 1, false, "depth ordering matches software");

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
    check(backend.render_headless_draw(blend_vk, blend_indices, vk_blend, vulkan, error), error.c_str());
    compare_frames(blend_software, vulkan, 1, true, "source-alpha blend matches software");
    check(backend.pipeline_count() == before_blend + 1u, "blend state creates one additional pipeline");

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
    check(backend.render_headless_draws(modulate_calls, vulkan, error), error.c_str());
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
    check(backend.render_headless_draw(modulate_vk, blend_indices, vk_fix, vulkan, error), error.c_str());
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
    check(backend.render_headless_draws(mask_calls, vulkan, error), error.c_str());
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
    check(backend.render_headless_draw(atest_vk, blend_indices, vk_atest, vulkan, error), error.c_str());
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
    check(backend.render_headless_draws(clamp_calls, vulkan, error), error.c_str());
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
    check(backend.render_headless_draw(fog_quad.vk, fog_quad.indices, vk_fog, vulkan, error), error.c_str());
    compare_frames(fog_software, vulkan, 1, false, "fog mixes toward the fog color on both");

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
    check(backend.render_headless_draw(solid.vk, solid.indices, vk_scissor, vulkan, error), error.c_str());
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
    check(backend.render_headless_draw(textured.vk, textured.indices, vk_texture, vulkan, error), error.c_str());
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
    check(backend.render_headless_draw(textured.vk, textured.indices, vk_texture, vulkan, error), error.c_str());
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
        check(backend.render_headless_draw(quad.vk, quad.indices, state, frame, error), contract.c_str());
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
        check(backend.render_headless_draw(quad.vk, quad.indices, state, frame, error), error);
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

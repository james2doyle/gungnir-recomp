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

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void shader_validation() {
    check(shaders::triangle_vert_size > 4u && shaders::triangle_frag_size > 4u, "both shader modules are embedded");
    check(shaders::triangle_vert[0] == 0x07230203u && shaders::triangle_frag[0] == 0x07230203u,
          "shader modules start with the SPIR-V magic");
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

int main() {
    try {
        shader_validation();
        headless_triangle();
        parity_tests();
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

// T7e contract tests: embedded shaders, headless Vulkan triangle readback and
// windowed swapchain creation/recreation.

#include "ge/vulkan_backend.hpp"
#include "ge/vulkan_shaders.hpp"

#if defined(GUNGNIR_HAVE_SDL3)
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

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

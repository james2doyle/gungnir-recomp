#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_vulkan.h>

#include "ge/vulkan_backend.hpp"

#include <string>

#if defined(__ANDROID__)
#include <android/log.h>
#define GUNGNIR_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "gungnir", __VA_ARGS__)
#define GUNGNIR_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "gungnir", __VA_ARGS__)
#else
#include <cstdio>
#define GUNGNIR_LOGI(...) std::fprintf(stdout, __VA_ARGS__)
#define GUNGNIR_LOGE(...) std::fprintf(stderr, __VA_ARGS__)
#endif

int main(int, char **) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        GUNGNIR_LOGE("sdl_init_failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *window =
        SDL_CreateWindow("Gungnir", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (window == nullptr) {
        GUNGNIR_LOGE("sdl_window_failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 extension_count = 0;
    const char *const *extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (extensions == nullptr || extension_count == 0) {
        GUNGNIR_LOGE("sdl_vulkan_extensions_failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    gungnir::ge::VulkanBackend backend;
    gungnir::ge::VulkanBackend::Config config;
    config.width = 1280;
    config.height = 720;
    config.headless = false;
    config.instance_extensions.assign(extensions, extensions + extension_count);
    config.create_surface = [window](VkInstance instance) -> VkSurfaceKHR {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
            GUNGNIR_LOGE("sdl_surface_failed: %s\n", SDL_GetError());
            return VK_NULL_HANDLE;
        }
        return surface;
    };

    std::string error;
    if (!backend.initialize(config, error)) {
        GUNGNIR_LOGE("vulkan_init_failed: %s\n", error.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    bool running = true;
    bool first_frame = true;
    int width = config.width;
    int height = config.height;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if (event.type == SDL_EVENT_WINDOW_RESIZED && event.window.data1 > 0 &&
                       event.window.data2 > 0) {
                width = event.window.data1;
                height = event.window.data2;
                if (!backend.recreate_swapchain(width, height, error)) {
                    GUNGNIR_LOGE("swapchain_resize_failed: %s\n", error.c_str());
                }
            }
        }
        if (!running) break;

        if (!backend.render_swapchain(error)) {
            if (error == "swapchain_out_of_date") {
                if (!backend.recreate_swapchain(width, height, error)) {
                    GUNGNIR_LOGE("swapchain_recreate_failed: %s\n", error.c_str());
                    break;
                }
                continue;
            }
            GUNGNIR_LOGE("render_failed: %s\n", error.c_str());
            break;
        }
        if (first_frame) {
            GUNGNIR_LOGI("first frame %dx%d\n", width, height);
            first_frame = false;
        }
        SDL_Delay(16);
    }

    backend.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

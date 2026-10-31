#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>

#include "ge/vulkan_backend.hpp"
#include "platform/android/assets.hpp"
#include "platform/android/game_data.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#include <jni.h>
#define GUNGNIR_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "gungnir", __VA_ARGS__)
#define GUNGNIR_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "gungnir", __VA_ARGS__)
#else
#include <cstdio>
#define GUNGNIR_LOGI(...) std::fprintf(stdout, __VA_ARGS__)
#define GUNGNIR_LOGE(...) std::fprintf(stderr, __VA_ARGS__)
#endif

namespace {

std::string read_asset(const char *name) {
    SDL_IOStream *stream = SDL_IOFromFile(name, "rb");
    if (stream == nullptr) return {};
    std::string data;
    char buffer[4096];
    std::size_t read = 0;
    while ((read = SDL_ReadIO(stream, buffer, sizeof(buffer))) > 0)
        data.append(buffer, read);
    SDL_CloseIO(stream);
    return data;
}

void unpack_runtime_assets(const std::filesystem::path &internal) {
    if (internal.empty()) return;
    const std::string expected = read_asset("runtime/version.txt");
    if (expected.empty()) {
        GUNGNIR_LOGI("runtime_assets_missing\n");
        return;
    }
    const std::filesystem::path destination = internal / "runtime" / "version.txt";
    std::ifstream existing(destination);
    const std::string current((std::istreambuf_iterator<char>(existing)),
                              std::istreambuf_iterator<char>());
    if (gungnir::platform::version_stamp_matches(current, expected)) {
        GUNGNIR_LOGI("runtime_assets_current\n");
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(expected.data(), static_cast<std::streamsize>(expected.size()));
    output.close();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        GUNGNIR_LOGE("runtime_assets_write_failed: %s\n", error.message().c_str());
        return;
    }
    GUNGNIR_LOGI("runtime_assets_unpacked\n");
}

gungnir::platform::GameDataResult check_game_data() {
    std::vector<std::filesystem::path> directories;
    if (const char *internal = SDL_GetAndroidInternalStoragePath(); internal != nullptr)
        directories.emplace_back(internal);
    if (const char *external = SDL_GetAndroidExternalStoragePath(); external != nullptr)
        directories.emplace_back(external);
    gungnir::platform::GameDataResult result = gungnir::platform::select_game_data(
        directories, gungnir::platform::kGungnirUsaIdentity);
    if (result.ok) {
        GUNGNIR_LOGI("game_data_ok origin=%s size=%llu crc=%08X\n", result.origin.c_str(),
                     static_cast<unsigned long long>(result.size),
                     static_cast<unsigned>(result.crc32));
    } else {
        GUNGNIR_LOGE("game_data_unavailable: %s\n", result.error.c_str());
    }
    return result;
}

void show_game_data_picker() {
#if defined(__ANDROID__)
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) return;
    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class == nullptr) return;
    jmethodID method = env->GetMethodID(activity_class, "showGameDataPicker", "()V");
    if (method != nullptr) env->CallVoidMethod(activity, method);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(activity_class);
#endif
}

} // namespace

#if defined(__ANDROID__)
extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeGameDataImported(JNIEnv *env, jobject, jstring path) {
    if (path == nullptr) return JNI_FALSE;
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (utf == nullptr) return JNI_FALSE;
    const std::string selected(utf);
    env->ReleaseStringUTFChars(path, utf);
    gungnir::platform::GameDataResult result =
        gungnir::platform::verify_game_data(selected, gungnir::platform::kGungnirUsaIdentity);
    result.origin = "saf";
    if (result.ok) {
        GUNGNIR_LOGI("game_data_ok origin=saf size=%llu crc=%08X\n",
                     static_cast<unsigned long long>(result.size),
                     static_cast<unsigned>(result.crc32));
    } else {
        GUNGNIR_LOGE("game_data_invalid: %s\n", result.error.c_str());
    }
    return result.ok ? JNI_TRUE : JNI_FALSE;
}
#endif

int main(int, char **) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        GUNGNIR_LOGE("sdl_init_failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (internal != nullptr) unpack_runtime_assets(internal);
    if (!check_game_data().ok) show_game_data_picker();
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

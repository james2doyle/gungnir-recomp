#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>

#include "ge/vulkan_backend.hpp"
#include "platform/android/affinity.hpp"
#include "platform/android/assets.hpp"
#include "platform/android/game_data.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
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

struct LifecycleState {
    std::atomic<bool> background{false};
    std::atomic<bool> resume{false};
    std::atomic<SDL_AudioStream *> tone{nullptr};
};

bool lifecycle_watch(void *userdata, SDL_Event *event) {
    auto *state = static_cast<LifecycleState *>(userdata);
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        state->background.store(true);
        if (SDL_AudioStream *tone = state->tone.load(); tone != nullptr) {
            if (!SDL_PauseAudioStreamDevice(tone))
                GUNGNIR_LOGE("audio_pause_failed: %s\n", SDL_GetError());
            else
                GUNGNIR_LOGI("audio_paused\n");
        }
        GUNGNIR_LOGI("lifecycle_paused\n");
    }
    if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        state->background.store(false);
        state->resume.store(true);
        if (SDL_AudioStream *tone = state->tone.load(); tone != nullptr) {
            if (!SDL_ResumeAudioStreamDevice(tone))
                GUNGNIR_LOGE("audio_resume_failed: %s\n", SDL_GetError());
            else
                GUNGNIR_LOGI("audio_resumed\n");
        }
        GUNGNIR_LOGI("lifecycle_event foreground\n");
    }
    return true;
}

double g_tone_phase = 0.0;

void tone_callback(void *, SDL_AudioStream *stream, int additional, int) {
    if (stream == nullptr || additional <= 0) return;
    std::vector<float> samples(static_cast<std::size_t>(additional) / sizeof(float));
    for (float &sample : samples) {
        sample = 0.05f * static_cast<float>(std::sin(g_tone_phase));
        g_tone_phase += 2.0 * 3.141592653589793 * 440.0 / 48000.0;
        if (g_tone_phase > 2.0 * 3.141592653589793) g_tone_phase -= 2.0 * 3.141592653589793;
    }
    SDL_PutAudioStreamData(stream, samples.data(),
                           static_cast<int>(samples.size() * sizeof(float)));
}

SDL_AudioStream *open_tone() {
    SDL_AudioSpec spec{SDL_AUDIO_F32, 1, 48000};
    SDL_AudioStream *stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, tone_callback, nullptr);
    if (stream == nullptr) {
        GUNGNIR_LOGE("audio_unavailable: %s\n", SDL_GetError());
        return nullptr;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        GUNGNIR_LOGE("audio_resume_failed: %s\n", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return nullptr;
    }
    GUNGNIR_LOGI("audio_tone_ready\n");
    return stream;
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        GUNGNIR_LOGE("sdl_init_failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (internal != nullptr) unpack_runtime_assets(internal);
    if (!check_game_data().ok) show_game_data_picker();
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft");
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

    const gungnir::platform::CpuTuning tuning = gungnir::platform::tune_current_thread();
    if (tuning.affinity_ok) {
        GUNGNIR_LOGI("affinity_applied cpu=%d\n", tuning.cpu);
    } else {
        GUNGNIR_LOGI("affinity_unavailable: %s\n", tuning.affinity_error.c_str());
    }
    if (tuning.priority_ok) {
        GUNGNIR_LOGI("priority_applied nice=%d\n", tuning.nice_value);
    } else {
        GUNGNIR_LOGI("priority_unavailable: %s\n", tuning.priority_error.c_str());
    }

    LifecycleState lifecycle;
    SDL_AudioStream *tone = open_tone();
    lifecycle.tone.store(tone);
    SDL_AddEventWatch(lifecycle_watch, &lifecycle);

    bool running = true;
    bool first_frame = true;
    bool needs_surface = false;
    int render_failures = 0;
    Uint64 next_surface_retry = 0;
    int width = config.width;
    int height = config.height;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT ||
                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                running = false;
            } else if ((event.type == SDL_EVENT_WINDOW_RESIZED ||
                        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
                       !lifecycle.background.load()) {
                int window_width = 0;
                int window_height = 0;
                SDL_GetWindowSizeInPixels(window, &window_width, &window_height);
                if (window_width > 0 && window_height > 0) {
                    width = window_width;
                    height = window_height;
                    if (!backend.recreate_swapchain(width, height, error)) {
                        GUNGNIR_LOGE("swapchain_resize_failed: %s\n", error.c_str());
                    }
                }
            }
        }
        if (!running) break;

        if (lifecycle.background.load()) {
            SDL_Delay(16);
            continue;
        }
        if (lifecycle.resume.exchange(false)) needs_surface = true;
        if (needs_surface) {
            const Uint64 now = SDL_GetTicks();
            if (now >= next_surface_retry) {
                next_surface_retry = now + 500;
                int window_width = 0;
                int window_height = 0;
                SDL_GetWindowSizeInPixels(window, &window_width, &window_height);
                if (window_width > 0 && window_height > 0) {
                    width = window_width;
                    height = window_height;
                }
                if (backend.recreate_surface(error)) {
                    needs_surface = false;
                    render_failures = 0;
                    GUNGNIR_LOGI("lifecycle_resumed %dx%d\n", width, height);
                } else {
                    GUNGNIR_LOGE("surface_recreate_failed: %s\n", error.c_str());
                }
            }
            SDL_Delay(16);
            continue;
        }

        if (!backend.render_swapchain(error)) {
            GUNGNIR_LOGE("render_failed: %s\n", error.c_str());
            needs_surface = true;
            if (++render_failures >= 3) break;
            SDL_Delay(100);
            continue;
        }
        render_failures = 0;
        if (first_frame) {
            GUNGNIR_LOGI("first frame %dx%d\n", width, height);
            first_frame = false;
        }
        SDL_Delay(16);
    }

    SDL_RemoveEventWatch(lifecycle_watch, &lifecycle);
    lifecycle.tone.store(nullptr);
    if (tone != nullptr) SDL_DestroyAudioStream(tone);
    backend.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

#include "ge/frame_queue.hpp"
#include "ge/state_machine.hpp"
#include "ge/targets.hpp"
#include "ge/vulkan_backend.hpp"
#include "hle/hle_media.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>

#include "audio/audio_mixer.hpp"
#include "input/input_state.hpp"
#include "input/menu_navigation.hpp"
#include "input/sdl_input.hpp"
#include "input/touch_haptics.hpp"
#include "input/touch_layout_store.hpp"
#include "input/touch_overlay.hpp"
#include "platform/adreno/package.hpp"
#include "platform/android/affinity.hpp"
#include "platform/android/assets.hpp"
#include "platform/android/audio_smoke.hpp"
#include "platform/android/aaudio_out.hpp"
#include "platform/android/game_data.hpp"
#include "platform/sdl_gamepad.hpp"
#include "session/game_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

std::mutex g_driver_info_mutex;
std::string g_driver_info;
std::mutex g_touch_mutex;
std::atomic<gungnir::platform::SdlGamepad *> g_gamepad{nullptr};

// Snapshot menu wiring (T15d). The session runs on a worker thread; menu
// Save/Load stop it, join it, then save/load/restart under this mutex. The
// snapshot holds the whole guest, so no guest state is touched mid-dispatch.
std::mutex g_snapshot_mutex;
gungnir::session::GameSession *g_snapshot_session = nullptr;
std::thread *g_snapshot_thread = nullptr;
std::atomic<bool> *g_snapshot_finished = nullptr;

// Host sinks installed once at startup (GE frame sink) but cleared by
// GameSession::start()'s HLE reset. Restart paths must re-arm them after a
// successful restart, or the rebooted guest renders into the void: frames
// flow nowhere, the screen freezes on a stale frame, and no guest_frame line
// ever logs (T15e device finding).
std::function<void()> g_session_rearm;

void run_session_worker(gungnir::session::GameSession &session, std::atomic<bool> &finished) {    session.run();
    const gungnir::session::Result &result = session.result();
    GUNGNIR_LOGI("%s\n", gungnir::session::summary_line(result).c_str());
    GUNGNIR_LOGI("session_stop_reason=%s\n", result.stop_reason.c_str());
    for (const std::string &import : result.imports_unbound)
        GUNGNIR_LOGI("session_unbound=%s\n", import.c_str());
    if (!result.ok) GUNGNIR_LOGE("session_error: %s\n", result.error.c_str());
    finished.store(true);
}

void launch_session_thread(gungnir::session::GameSession &session, std::atomic<bool> &finished,
    std::thread &slot) {
    finished.store(false);
    slot = std::thread([&session, &finished] { run_session_worker(session, finished); });
}

gungnir::input::TouchHaptics &touch_haptics() {
    static gungnir::input::TouchHaptics haptics([] {});
    return haptics;
}

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

bool write_runtime_asset(const std::filesystem::path &destination, const std::string &content) {
    std::ifstream existing(destination, std::ios::binary);
    const std::string current((std::istreambuf_iterator<char>(existing)),
                              std::istreambuf_iterator<char>());
    if (current == content) return false;

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    const std::filesystem::path temporary = destination.string() + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        GUNGNIR_LOGE("runtime_assets_write_failed: %s\n", error.message().c_str());
        return false;
    }
    return true;
}

void unpack_runtime_assets(const std::filesystem::path &internal) {
    if (internal.empty()) return;
    const std::string version = read_asset("runtime/version.txt");
    const std::string nids = read_asset("nids.csv");
    if (version.empty()) {
        GUNGNIR_LOGI("runtime_assets_missing\n");
        return;
    }
    if (nids.empty()) GUNGNIR_LOGE("runtime_assets_missing_nids\n");

    const std::filesystem::path runtime = internal / "runtime";
    bool changed = write_runtime_asset(runtime / "version.txt", version);
    if (!nids.empty()) changed = write_runtime_asset(runtime / "nids.csv", nids) || changed;
    if (changed) {
        GUNGNIR_LOGI("runtime_assets_unpacked\n");
    } else {
        GUNGNIR_LOGI("runtime_assets_current\n");
    }
}

std::filesystem::path find_runtime_elf(const std::vector<std::filesystem::path> &directories) {
    for (const auto &directory : directories) {
        const std::filesystem::path candidate = directory / "game" / "game.elf";
        if (!std::filesystem::is_regular_file(candidate)) continue;
        std::ifstream input(candidate, std::ios::binary);
        char magic[4]{};
        input.read(magic, sizeof(magic));
        if (input.gcount() != static_cast<std::streamsize>(sizeof(magic)) ||
            magic[0] != '\x7f' || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
            GUNGNIR_LOGE("game_elf_invalid_magic: %s\n", candidate.string().c_str());
            continue;
        }
        return candidate;
    }
    return {};
}

class SilentSink final : public gungnir::audio::AudioSink {
public:
    bool submit(const gungnir::audio::AudioBuffer &buffer) override {
        buffers += buffer.frames;
        return true;
    }
    std::uint64_t buffers{};
};

#if defined(__ANDROID__)
long long intent_long(const char *key, long long fallback) {
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) return fallback;
    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class == nullptr) return fallback;
    jmethodID get_intent = env->GetMethodID(activity_class, "getIntent",
                                            "()Landroid/content/Intent;");
    jobject intent = get_intent != nullptr ? env->CallObjectMethod(activity, get_intent) : nullptr;
    long long value = fallback;
    if (intent != nullptr) {
        jclass intent_class = env->GetObjectClass(intent);
        jmethodID get_long = intent_class != nullptr
            ? env->GetMethodID(intent_class, "getLongExtra", "(Ljava/lang/String;J)J") : nullptr;
        jstring name = env->NewStringUTF(key);
        if (get_long != nullptr && name != nullptr)
            value = static_cast<long long>(
                env->CallLongMethod(intent, get_long, name, static_cast<jlong>(fallback)));
        if (name != nullptr) env->DeleteLocalRef(name);
        if (intent_class != nullptr) env->DeleteLocalRef(intent_class);
        env->DeleteLocalRef(intent);
    }
    env->DeleteLocalRef(activity_class);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return value;
}
#else
long long intent_long(const char *, long long fallback) {
    return fallback;
}
#endif

#if defined(__ANDROID__)
std::string intent_string(const char *key) {
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) return {};
    jclass activity_class = env->GetObjectClass(activity);
    if (activity_class == nullptr) return {};
    jmethodID get_intent = env->GetMethodID(activity_class, "getIntent",
                                            "()Landroid/content/Intent;");
    jobject intent = get_intent != nullptr ? env->CallObjectMethod(activity, get_intent) : nullptr;
    std::string value;
    if (intent != nullptr) {
        jclass intent_class = env->GetObjectClass(intent);
        jmethodID get_string = intent_class != nullptr ? env->GetMethodID(intent_class, "getStringExtra",
            "(Ljava/lang/String;)Ljava/lang/String;") : nullptr;
        jstring name = env->NewStringUTF(key);
        jobject extra = (get_string != nullptr && name != nullptr)
            ? env->CallObjectMethod(intent, get_string, name) : nullptr;
        if (extra != nullptr) {
            const char *utf = env->GetStringUTFChars(static_cast<jstring>(extra), nullptr);
            if (utf != nullptr) {
                value = utf;
                env->ReleaseStringUTFChars(static_cast<jstring>(extra), utf);
            }
            env->DeleteLocalRef(extra);
        }
        if (name != nullptr) env->DeleteLocalRef(name);
        if (intent_class != nullptr) env->DeleteLocalRef(intent_class);
        env->DeleteLocalRef(intent);
    }
    env->DeleteLocalRef(activity_class);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return value;
}
#endif

bool present_guest_frame(gungnir::ge::VulkanBackend &backend,
                         const std::vector<gungnir::ge::Draw> &draws, std::string &error) {
    // The display buffer selects the presented target; anything else is
    // offscreen (sampled via render-to-texture or ignored).
    const std::uint32_t framebuffer = gungnir::hle::display_state().framebuffer;
    const std::uint32_t display_offset =
        framebuffer >= gungnir::ge::kVramBase ? framebuffer - gungnir::ge::kVramBase : 0u;
    struct OwnedDraw {
        std::vector<gungnir::ge::VulkanDrawVertex> vertices;
        std::vector<std::uint16_t> indices;
        gungnir::ge::VulkanDrawState state;
    };
    std::vector<OwnedDraw> owned(draws.size());
    std::vector<gungnir::ge::VulkanDrawCall> calls;
    calls.reserve(draws.size());
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const gungnir::ge::Draw &source = draws[i];
        owned[i].vertices.reserve(source.vertices.size());
        for (const gungnir::ge::ScreenVertex &vertex : source.vertices)
            owned[i].vertices.push_back(gungnir::ge::to_vulkan_vertex(vertex));
        owned[i].indices = source.indices;
        const gungnir::ge::DrawState &in = source.state;
        gungnir::ge::VulkanDrawState &out = owned[i].state;
        out.scissor_x0 = in.scissor_x0;
        out.scissor_y0 = in.scissor_y0;
        out.scissor_x1 = in.scissor_x1;
        out.scissor_y1 = in.scissor_y1;
        // Clear draws keep the depth test enabled with ALWAYS: Vulkan
        // disables depth writes whenever the test is off, so test-off /
        // write-on would leave the plane at the render-pass clear value
        // and reject every GREATER-tested draw. Vertex z carries the
        // clear depth; the 0xD3 depth bit still gates the write.
        out.depth_test = in.clear ? true : in.depth_test;
        out.depth_function =
            in.clear ? gungnir::ge::CompareFunc::Always : in.depth_function;
        out.depth_write = in.clear ? in.clear_depth_write : in.depth_write;
        out.cull = in.cull;
        out.front_face = in.front_face;
        out.blend = in.blend;
        out.alpha_test = in.alpha_test;
        out.alpha_function = in.alpha_function;
        out.alpha_reference = in.alpha_reference;
        out.fog_enabled = in.fog_enabled;
        out.fog_color = in.fog_color;
        out.tex_func = in.tex_func;
        out.tex_rgba = in.tex_rgba;
        out.color_double = in.color_double;
        out.tex_env = in.tex_env;
        out.fb_format = in.draw_format <= 3u ? static_cast<gungnir::ge::TextureFormat>(in.draw_format)
                                            : gungnir::ge::TextureFormat::Rgba8888;
        out.primitive = source.primitive;
        out.stencil = in.stencil;
        out.write_mask = in.write_mask;
        out.texture = in.texture;
        out.texture_key = in.texture_key;
        out.linear = in.linear;
        out.clamp_u = in.clamp_u;
        out.clamp_v = in.clamp_v;
        out.draw_buffer = in.draw_buffer;
        calls.push_back({owned[i].vertices, owned[i].indices, out});
    }
    return backend.render_swapchain_draws(calls, error, display_offset);
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
    gungnir::input::SdlInput *sdl_input{};
    gungnir::platform::SdlGamepad *gamepad{};
    gungnir::session::GameSession *session{};
    gungnir::audio::AAudioOut *audio_out{};
};

bool lifecycle_watch(void *userdata, SDL_Event *event) {
    auto *state = static_cast<LifecycleState *>(userdata);
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        if (state->sdl_input != nullptr) state->sdl_input->release_held();
        if (state->gamepad != nullptr) state->gamepad->release_all();
        gungnir::input::state().release_all();
        state->background.store(true);
        if (state->session != nullptr) {
            state->session->request_stop();
            GUNGNIR_LOGI("session_stop_requested reason=background\n");
        }
        if (state->audio_out != nullptr) {
            std::string error;
            if (!state->audio_out->pause(error))
                GUNGNIR_LOGE("audio_pause_failed: %s\n", error.c_str());
            else
                GUNGNIR_LOGI("audio_paused\n");
        }
        GUNGNIR_LOGI("lifecycle_paused\n");
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (state->sdl_input != nullptr) state->sdl_input->release_held();
        if (state->gamepad != nullptr) state->gamepad->release_all();
        gungnir::input::state().release_all();
    }
    if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
        state->background.store(false);
        state->resume.store(true);
        if (state->audio_out != nullptr) {
            std::string error;
            if (!state->audio_out->resume(error))
                GUNGNIR_LOGE("audio_resume_failed: %s\n", error.c_str());
            else
                GUNGNIR_LOGI("audio_resumed\n");
        }
        GUNGNIR_LOGI("lifecycle_event foreground\n");
    }
    return true;
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

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeSaveSnapshot(JNIEnv *env, jobject, jstring path) {
    if (path == nullptr) return env->NewStringUTF("snapshot_save_failed: null path");
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (utf == nullptr) return env->NewStringUTF("snapshot_save_failed: null path bytes");
    const std::string file(utf);
    env->ReleaseStringUTFChars(path, utf);
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snapshot_session == nullptr || g_snapshot_thread == nullptr || g_snapshot_finished == nullptr)
        return env->NewStringUTF("session_not_started");
    if (g_snapshot_finished->load()) return env->NewStringUTF("snapshot_save_failed: session finished");
    g_snapshot_session->request_stop();
    if (g_snapshot_thread->joinable()) g_snapshot_thread->join();
    std::string error;
    if (!g_snapshot_session->save_snapshot(file, error)) return env->NewStringUTF(error.c_str());
    GUNGNIR_LOGI("snapshot_saved path=%s\n", file.c_str());
    g_snapshot_session->request_resume();
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    return nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeLoadSnapshot(JNIEnv *env, jobject, jstring path) {
    if (path == nullptr) return env->NewStringUTF("snapshot_load_failed: null path");
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (utf == nullptr) return env->NewStringUTF("snapshot_load_failed: null path bytes");
    const std::string file(utf);
    env->ReleaseStringUTFChars(path, utf);
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snapshot_session == nullptr || g_snapshot_thread == nullptr || g_snapshot_finished == nullptr)
        return env->NewStringUTF("session_not_started");
    // Like reset-to-title: a finished session restarts from the snapshot
    // instead of refusing. Only join a live worker.
    if (!g_snapshot_finished->load()) {
        g_snapshot_session->request_stop();
        if (g_snapshot_thread->joinable()) g_snapshot_thread->join();
    } else if (g_snapshot_thread->joinable()) {
        g_snapshot_thread->join();
    }
    std::string error;
    if (!g_snapshot_session->load_snapshot(file, error)) return env->NewStringUTF(error.c_str());
    GUNGNIR_LOGI("snapshot_loaded path=%s\n", file.c_str());
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    return nullptr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeResetToTitle(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    if (g_snapshot_session == nullptr || g_snapshot_thread == nullptr || g_snapshot_finished == nullptr)
        return env->NewStringUTF("session_not_started");
    // Only join a live worker; a finished session restarts without one.
    if (!g_snapshot_finished->load()) {
        g_snapshot_session->request_stop();
        if (g_snapshot_thread->joinable()) g_snapshot_thread->join();
    } else if (g_snapshot_thread->joinable()) {
        g_snapshot_thread->join();
    }
    std::string error;
    if (!g_snapshot_session->restart_to_title(error)) return env->NewStringUTF(error.c_str());
    if (g_session_rearm) g_session_rearm();
    GUNGNIR_LOGI("session_reset_to_title\n");
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    return nullptr;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeAudioSmoke(JNIEnv *, jobject, jint duration_ms) {
    const gungnir::platform::AudioSmokeResult result =
        gungnir::platform::run_audio_smoke(static_cast<std::uint32_t>(duration_ms));
    return result.ok ? JNI_TRUE : JNI_FALSE;
}

std::string jstring_to_string(JNIEnv *env, jstring value) {
    if (value == nullptr) return {};
    const char *utf = env->GetStringUTFChars(value, nullptr);
    if (utf == nullptr) return {};
    std::string result(utf);
    env->ReleaseStringUTFChars(value, utf);
    return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeImportDriver(JNIEnv *env, jobject, jstring zip_path,
                                                           jstring drivers_root) {
    const std::string zip = jstring_to_string(env, zip_path);
    const std::string root = jstring_to_string(env, drivers_root);
    if (zip.empty() || root.empty()) return env->NewStringUTF("driver_import_args");
    const gungnir::platform::DriverInspectResult result =
        gungnir::platform::install_driver_zip(zip, root);
    if (result.ok) {
        GUNGNIR_LOGI("driver_import_ok id=%s soname=%s\n", result.package.id.c_str(),
                     result.package.soname.c_str());
        return env->NewStringUTF("");
    }
    GUNGNIR_LOGE("driver_import_failed: %s\n", result.error.c_str());
    return env->NewStringUTF(result.error.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeRemoveDriver(JNIEnv *env, jobject,
                                                           jstring drivers_root, jstring id) {
    const std::string root = jstring_to_string(env, drivers_root);
    const std::string driver_id = jstring_to_string(env, id);
    std::string error;
    if (root.empty() || !gungnir::platform::remove_driver(root, driver_id, error)) {
        if (error.empty()) error = "driver_storage_unavailable";
        GUNGNIR_LOGE("driver_remove_failed: %s\n", error.c_str());
        return env->NewStringUTF(error.c_str());
    }
    GUNGNIR_LOGI("driver_remove_ok id=%s\n", driver_id.c_str());
    return env->NewStringUTF("");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeDriverInfo(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_driver_info_mutex);
    return env->NewStringUTF(g_driver_info.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeViewport(
    JNIEnv *, jclass, jint width, jint height, jint left, jint top, jint right, jint bottom) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    gungnir::input::touch_overlay().set_viewport(width, height,
        gungnir::input::TouchInsets{static_cast<float>(left), static_cast<float>(top),
            static_cast<float>(right), static_cast<float>(bottom)});
}

extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeTouch(
    JNIEnv *, jclass, jint action, jint pointer, jfloat x, jfloat y) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    auto &overlay = gungnir::input::touch_overlay();
    bool tick = false;
    switch (action) {
    case 0:
        tick = touch_haptics().press(overlay.touch_down(pointer, x, y));
        break;
    case 1:
        overlay.touch_move(pointer, x, y);
        break;
    case 2:
        overlay.touch_up(pointer);
        break;
    default:
        overlay.cancel();
        break;
    }
    overlay.publish();
    return tick ? 1 : 0;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeSnapshot(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    const auto snapshot = gungnir::input::touch_overlay().snapshot();
    std::array<jfloat, gungnir::input::kTouchControls * 5u> values{};
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        values[i * 5u + 0u] = snapshot[i].cx;
        values[i * 5u + 1u] = snapshot[i].cy;
        values[i * 5u + 2u] = snapshot[i].radius;
        values[i * 5u + 3u] = snapshot[i].visible ? 1.0f : 0.0f;
        values[i * 5u + 4u] = snapshot[i].pressed ? 1.0f : 0.0f;
    }
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(values.size()));
    if (result != nullptr)
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeDpadBits(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    return static_cast<jint>(gungnir::input::touch_overlay().dpad_bits());
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeStick(JNIEnv *env, jclass) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    const gungnir::input::ActiveStick stick = gungnir::input::touch_overlay().active_stick();
    const std::array<jfloat, 6u> values{stick.active ? 1.0f : 0.0f, stick.origin_x, stick.origin_y,
        stick.thumb_x, stick.thumb_y, stick.reach};
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(values.size()));
    if (result != nullptr)
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(values.size()), values.data());
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeEditing(JNIEnv *, jclass, jboolean editing) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    gungnir::input::touch_overlay().set_editing(editing == JNI_TRUE);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeLoadLayout(JNIEnv *env, jclass, jstring path) {
    const std::string file = jstring_to_string(env, path);
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    const gungnir::input::TouchLayoutResult result =
        gungnir::input::load_touch_layout(file, gungnir::input::touch_overlay());
    switch (result.status) {
    case gungnir::input::TouchLayoutStatus::Ok:
        return 0;
    case gungnir::input::TouchLayoutStatus::Missing:
        return 1;
    case gungnir::input::TouchLayoutStatus::Older:
        return 2;
    case gungnir::input::TouchLayoutStatus::Corrupt:
        return 3;
    default:
        return 4;
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeSaveLayout(JNIEnv *env, jclass, jstring path) {
    const std::string file = jstring_to_string(env, path);
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    const gungnir::input::TouchLayoutResult result =
        gungnir::input::save_touch_layout(file, gungnir::input::touch_overlay());
    return result.status == gungnir::input::TouchLayoutStatus::Ok ? 0 : 1;
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_TouchOverlayView_nativeResetLayout(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lock(g_touch_mutex);
    auto &overlay = gungnir::input::touch_overlay();
    overlay.reset_layout();
    overlay.publish();
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeMenuBegin(JNIEnv *, jobject, jint rows) {
    gungnir::input::menu_navigation().begin(static_cast<int>(rows));
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeMenuEnd(JNIEnv *, jobject) {
    gungnir::input::menu_navigation().end();
}

// Packed for the Java poll loop: action in the low byte, index in the next one.
extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeMenuPoll(JNIEnv *, jobject) {
    const std::optional<gungnir::input::MenuEvent> event =
        gungnir::input::menu_navigation().take();
    if (!event) return 0;
    GUNGNIR_LOGI("menu_action=%d index=%d\n", static_cast<int>(event->action), event->index);
    return static_cast<jint>(event->action) | (static_cast<jint>(event->index) << 8);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_active(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    return gamepad != nullptr && gamepad->active() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_guideAvailable(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    return gamepad != nullptr && gamepad->guide_available() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_needsRemap(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    return gamepad != nullptr && gamepad->needs_remap() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_info(JNIEnv *env, jclass) {
    auto *gamepad = g_gamepad.load();
    return env->NewStringUTF(gamepad != nullptr ? gamepad->info().c_str() : "No gamepad connected");
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_beginRemap(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    if (gamepad != nullptr) gamepad->begin_remap();
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_cancelRemap(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    if (gamepad != nullptr) gamepad->cancel_remap();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_remapStatus(JNIEnv *env, jclass) {
    auto *gamepad = g_gamepad.load();
    return env->NewStringUTF(gamepad != nullptr ? gamepad->remap_status().c_str()
                                                : "No gamepad connected");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_remapDone(JNIEnv *, jclass) {
    auto *gamepad = g_gamepad.load();
    return gamepad != nullptr && gamepad->remap_done() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_saveRemap(JNIEnv *env, jclass) {
    auto *gamepad = g_gamepad.load();
    if (gamepad == nullptr) return env->NewStringUTF("gamepad_unavailable");
    std::string error;
    return env->NewStringUTF(gamepad->save_remap(error) ? "" : error.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_input_GamepadBridge_resetProfile(JNIEnv *env, jclass) {
    auto *gamepad = g_gamepad.load();
    if (gamepad == nullptr) return env->NewStringUTF("gamepad_unavailable");
    std::string error;
    return env->NewStringUTF(gamepad->reset_profile(error) ? "" : error.c_str());
}
#endif

int main(int, char **) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        GUNGNIR_LOGE("sdl_init_failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (internal != nullptr) unpack_runtime_assets(internal);
    const std::string gamepad_profiles = read_asset("input_profiles/profiles.json");
    const std::filesystem::path gamepad_directory = internal != nullptr
        ? std::filesystem::path(internal) / "input_profiles"
        : std::filesystem::path("input_profiles");
    gungnir::platform::SdlGamepad gamepad(gamepad_profiles, gamepad_directory);
    g_gamepad.store(&gamepad);
    if (gamepad_profiles.empty()) GUNGNIR_LOGE("gamepad_profiles_asset_missing\n");
    gungnir::platform::GameDataResult game_data = check_game_data();
    if (!game_data.ok) show_game_data_picker();
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

    gungnir::platform::VulkanDriverSelection driver_selection;
    if (internal != nullptr) {
        std::string selection_error;
        const std::filesystem::path selection_path =
            std::filesystem::path(internal) / "drivers" / "selection.txt";
        if (!gungnir::platform::read_driver_selection(selection_path, driver_selection,
                                                      selection_error)) {
            GUNGNIR_LOGE("driver_selection_invalid: %s\n", selection_error.c_str());
            driver_selection = {};
        }
    }

    gungnir::ge::VulkanBackend backend;
    gungnir::ge::VulkanBackend::Config config;
    config.width = 480;
    config.height = 272;
    config.virtual_width = 480;
    config.virtual_height = 272;
    config.headless = false;
    config.driver = driver_selection;
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

    GUNGNIR_LOGI("swapchain=%dx%d transform=%u\n", backend.swapchain_width(), backend.swapchain_height(),
                 static_cast<unsigned>(backend.surface_transform()));
    const gungnir::platform::VulkanDriverInfo &driver = backend.driver_info();
    GUNGNIR_LOGI("driver_active=%s name=%s info=%s api=%u\n",
                 driver.custom ? "custom" : "system", driver.driver_name.c_str(),
                 driver.driver_info.c_str(), driver.api_version);
    if (driver.fell_back) {
        GUNGNIR_LOGE("driver_fallback: %s\n", driver.fallback_error.c_str());
    }
    {
        char info[1024];
        std::snprintf(info, sizeof(info),
                      "active=%s\nname=%s\ninfo=%s\napi=%u\n",
                      driver.custom ? "custom" : "system", driver.driver_name.c_str(),
                      driver.driver_info.c_str(), driver.api_version);
        std::lock_guard<std::mutex> lock(g_driver_info_mutex);
        g_driver_info = info;
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

    gungnir::audio::MixerSink mixer(44'100u);
    SilentSink silent_sink;
    gungnir::audio::AudioSink *audio_sink = &silent_sink;
    std::unique_ptr<gungnir::audio::AAudioOut> audio_out;
    {
        std::string audio_error;
        audio_out = gungnir::audio::AAudioOut::open(mixer, audio_error);
        if (audio_out != nullptr) {
            audio_sink = &mixer;
            GUNGNIR_LOGI("audio_output_ready\n");
        } else {
            GUNGNIR_LOGE("audio_output_unavailable: %s\n", audio_error.c_str());
        }
    }
    gungnir::audio::set_audio_sink(audio_sink);

    const bool debug_quad = intent_long("debug_quad", 0) != 0;
    gungnir::ge::FrameQueue frame_queue;
    gungnir::session::GameSession session;
    std::thread session_thread;
    std::atomic<bool> session_finished{false};
    if (debug_quad) {
        GUNGNIR_LOGI("debug_quad enabled\n");
    } else if (game_data.ok && internal != nullptr) {
        std::vector<std::filesystem::path> game_directories;
        game_directories.emplace_back(internal);
        if (const char *external = SDL_GetAndroidExternalStoragePath(); external != nullptr)
            game_directories.emplace_back(external);
        const std::filesystem::path elf = find_runtime_elf(game_directories);
        if (elf.empty()) {
            GUNGNIR_LOGE("session_start_failed: game_elf_missing (push game.elf to files/game/)\n");
        } else {
            gungnir::session::Config session_config;
            session_config.elf_path = elf;
            session_config.nids_path = std::filesystem::path(internal) / "runtime" / "nids.csv";
            session_config.disc_path = game_data.path;
            session_config.memory_stick = std::filesystem::path(internal) / "psp";
            session_config.max_dispatches = static_cast<std::uint64_t>(
                intent_long("dispatch_limit", 5'000'000'000LL));
            session_config.wall_seconds = static_cast<double>(
                intent_long("wall_seconds", 0));
            session_config.stall_seconds = static_cast<double>(
                intent_long("stall_seconds", 30));
            session_config.stop_at_flip = intent_long("stop_at_flip", 0) != 0;
            session_config.pacing = intent_long("pacing", 1) != 0;
            std::error_code directory_error;
            std::filesystem::create_directories(session_config.memory_stick, directory_error);
            session.set_diagnostic_sink([](std::string_view line) {
                GUNGNIR_LOGE("%.*s\n", static_cast<int>(line.size()), line.data());
            });
            std::string start_error;
            if (!session.start(session_config, audio_sink, start_error)) {
                GUNGNIR_LOGE("session_start_failed: %s\n", start_error.c_str());
            } else {
                GUNGNIR_LOGI("session_start elf=%s\n", elf.string().c_str());
                gungnir::hle::set_ge_frame_sink([&frame_queue](const std::vector<gungnir::ge::Draw> &draws) {
                    frame_queue.publish(draws);
                });
                {
                    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
                    g_snapshot_session = &session;
                    g_snapshot_thread = &session_thread;
                    g_snapshot_finished = &session_finished;
                    g_session_rearm = [&frame_queue] {
                        gungnir::hle::set_ge_frame_sink(
                            [&frame_queue](const std::vector<gungnir::ge::Draw> &draws) {
                                frame_queue.publish(draws);
                            });
                    };
                }
                const std::string snapshot_in = intent_string("snapshot_in");
                if (!snapshot_in.empty()) {
                    std::string snapshot_error;
                    if (!session.load_snapshot(snapshot_in, snapshot_error)) {
                        GUNGNIR_LOGE("session_snapshot_in_failed: %s\n", snapshot_error.c_str());
                    } else {
                        GUNGNIR_LOGI("session_snapshot_in=%s\n", snapshot_in.c_str());
                    }
                }
                launch_session_thread(session, session_finished, session_thread);
            }
        }
    } else if (!game_data.ok) {
        GUNGNIR_LOGE("session_start_failed: game_data_unavailable\n");
    }

    LifecycleState lifecycle;
    gungnir::input::SdlInput sdl_input;
    lifecycle.sdl_input = &sdl_input;
    lifecycle.gamepad = &gamepad;
    lifecycle.session = &session;
    lifecycle.audio_out = audio_out.get();
    SDL_AddEventWatch(lifecycle_watch, &lifecycle);

    bool running = true;
    bool first_frame = true;
    std::uint64_t guest_frames = 0u;
    bool needs_surface = false;
    int render_failures = 0;
    Uint64 next_surface_retry = 0;
    Uint64 next_audio_stats = SDL_GetTicks() + 5000;
    int width = config.width;
    int height = config.height;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            gamepad.handle_event(event);
            sdl_input.handle(event);
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
                    } else {
                        GUNGNIR_LOGI("swapchain=%dx%d transform=%u\n", backend.swapchain_width(),
                                     backend.swapchain_height(),
                                     static_cast<unsigned>(backend.surface_transform()));
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

        std::vector<gungnir::ge::Draw> guest_draws;
        const bool have_guest_frame = !debug_quad && frame_queue.take_latest(guest_draws);
        bool rendered = false;
        if (debug_quad) {
            const std::array<gungnir::ge::VulkanDrawVertex, 4> vertices{{
                {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
                {480.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
                {0.0f, 272.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
                {480.0f, 272.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            }};
            const std::array<std::uint16_t, 6> indices{{0, 1, 2, 2, 1, 3}};
            gungnir::ge::VulkanDrawState state;
            state.scissor_x0 = 0;
            state.scissor_y0 = 0;
            state.scissor_x1 = 480;
            state.scissor_y1 = 272;
            const std::array<gungnir::ge::VulkanDrawCall, 1> calls{{{vertices, indices, state}}};
            rendered = backend.render_swapchain_draws(calls, error, 0u);
        } else if (have_guest_frame) {
            rendered = present_guest_frame(backend, guest_draws, error);
        } else {
            SDL_Delay(1);
            continue;
        }
        if (!rendered) {
            GUNGNIR_LOGE("render_failed: %s\n", error.c_str());
            needs_surface = true;
            if (++render_failures >= 3) break;
            SDL_Delay(100);
            continue;
        }
        render_failures = 0;
        if (have_guest_frame && !guest_draws.empty()) {
            ++guest_frames;
            std::size_t vertices = 0u;
            for (const gungnir::ge::Draw &draw : guest_draws) vertices += draw.vertices.size();
            if (guest_frames == 1u || guest_frames % 60u == 0u)
                GUNGNIR_LOGI("guest_frame=%llu draws=%zu vertices=%zu ge_gaps=%llu\n",
                             static_cast<unsigned long long>(guest_frames), guest_draws.size(), vertices,
                             static_cast<unsigned long long>(gungnir::hle::ge_gap_count()));
            if (guest_frames == 1u || guest_frames == 60u) {
                const auto &display = gungnir::hle::display_state();
                GUNGNIR_LOGI("guest_display=%ux%u fb=%08X width=%u format=%u\n", display.width,
                             display.height, display.framebuffer, display.buffer_width, display.pixel_format);
                const std::size_t shown = std::min<std::size_t>(guest_draws.size(), 12u);
                for (std::size_t i = 0; i < shown; ++i) {
                    const gungnir::ge::Draw &draw = guest_draws[i];
                    const gungnir::ge::ScreenVertex *v =
                        draw.vertices.empty() ? nullptr : &draw.vertices.front();
                    GUNGNIR_LOGI("guest_draw=%zu verts=%zu indices=%zu scissor=%d,%d-%d,%d texture=%dx%d "
                                 "v0=%.1f,%.1f color=%.2f,%.2f,%.2f,%.2f\n",
                                 i, draw.vertices.size(), draw.indices.size(), draw.state.scissor_x0,
                                 draw.state.scissor_y0, draw.state.scissor_x1, draw.state.scissor_y1,
                                 draw.state.texture != nullptr ? draw.state.texture->width : 0,
                                 draw.state.texture != nullptr ? draw.state.texture->height : 0,
                                 v != nullptr ? v->x : 0.0f, v != nullptr ? v->y : 0.0f,
                                 v != nullptr ? v->color[0] : 0.0f, v != nullptr ? v->color[1] : 0.0f,
                                 v != nullptr ? v->color[2] : 0.0f, v != nullptr ? v->color[3] : 0.0f);
                }
            }
        }
        const Uint64 now_ticks = SDL_GetTicks();
        if (now_ticks >= next_audio_stats) {
            next_audio_stats = now_ticks + 5000;
            const gungnir::audio::MixerStats stats = mixer.stats();
            GUNGNIR_LOGI("audio_mixer pulled=%llu underrun=%llu submitted=%llu overruns=%llu rejected=%llu "
                         "peak=%u xruns=%d\n",
                         static_cast<unsigned long long>(stats.frames_pulled),
                         static_cast<unsigned long long>(stats.underrun_frames),
                         static_cast<unsigned long long>(stats.submitted_buffers),
                         static_cast<unsigned long long>(stats.overruns),
                         static_cast<unsigned long long>(stats.rejected_buffers), stats.peak,
                         audio_out != nullptr ? audio_out->xruns() : 0);
        }
        if (first_frame) {
            GUNGNIR_LOGI("first frame %dx%d\n", width, height);
            first_frame = false;
        }
        SDL_Delay(16);
    }

    SDL_RemoveEventWatch(lifecycle_watch, &lifecycle);
    session.request_stop();
    if (session_thread.joinable()) session_thread.join();
    gungnir::hle::set_ge_frame_sink(nullptr);
    session.shutdown();
    gungnir::audio::set_audio_sink(nullptr);
    audio_out.reset();
    g_gamepad.store(nullptr);
    gamepad.shutdown();
    backend.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

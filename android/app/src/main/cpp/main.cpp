#include "ge/frame_interpolation.hpp"
#include "ge/present_schedule.hpp"
#include "ge/frame_queue.hpp"
#include "ge/state_machine.hpp"
#include "ge/targets.hpp"
#include "ge/vulkan_backend.hpp"
#include "ge/widescreen_coverage.hpp"
#include "hle/cadence.hpp"
#include "hle/hle_media.hpp"
#include "hle/kernel.hpp"
#include "host/profile.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_vulkan.h>

#include "audio/audio_mixer.hpp"
#include "input/host_controls.hpp"
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
#include "platform/android/game_installer.hpp"
#include "platform/android/stack_space.hpp"
#include "platform/sdl_gamepad.hpp"
#include "session/game_session.hpp"
#include "session/session_lifecycle.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

// Per-title host patches (75yfmuis). Host state only: never snapshotted, never
// part of the snapshot identity. Values live in the gitignored filesDir path
// below and are opt-in; an unvalidated value is refused, never best-efforted.
gungnir::host::HostProfile g_host_profile;
const gungnir::host::HostProfile &host_profile() { return g_host_profile; }

// Snapshot menu wiring (T15d). The session runs on a worker thread; menu
// Save/Load stop it, join it, then save/load/restart under this mutex. The
// snapshot holds the whole guest, so no guest state is touched mid-dispatch.
std::mutex g_snapshot_mutex;
gungnir::session::GameSession *g_snapshot_session = nullptr;
std::thread *g_snapshot_thread = nullptr;
std::atomic<bool> *g_snapshot_finished = nullptr;
// A menu save/load/reset relaunches a live worker under the snapshot mutex.
// The activity's pending background pause is then moot; the main loop clears
// it on its next tick instead of resuming on top of the new worker.
std::atomic<bool> g_lifecycle_reset_requested{false};

// Host sinks installed once at startup (GE frame sink) but cleared by
// GameSession::start()'s HLE reset. Restart paths must re-arm them after a
// successful restart, or the rebooted guest renders into the void: frames
// flow nowhere, the screen freezes on a stale frame, and no guest_frame line
// ever logs (T15e device finding).
std::function<void()> g_session_rearm;

// A guest flip publishes its frame with the host moment it is due. Under the
// pacing hold the kernel maps the flip's emulated time to real time, so the
// presenter shows the frame when the guest produced it instead of one fixed
// sleep later. Unpaced runs have no mapping and present immediately.
gungnir::ge::FrameQueue::Clock::time_point frame_due_at() {
    const auto at = gungnir::hle::kernel().real_time_of(gungnir::hle::kernel().now_us());
    return at.value_or(gungnir::ge::FrameQueue::Clock::now());
}

// Renderer interpolation is host presentation policy: it never enters guest
// RAM, a snapshot or a save, so save states stay comparable with it on or off.
std::atomic<bool> g_interpolation_enabled{false};

// Display layout is the same kind of policy: how the 480x272 framebuffer is
// laid onto the display, never what the guest renders.
std::atomic<gungnir::ge::DisplayLayout> g_display_layout{gungnir::ge::DisplayLayout::Fit};

// Host widescreen widening factor; 1 is the native 480x272 frame.
double g_widescreen_scale = 1.0;

const char *display_layout_name(gungnir::ge::DisplayLayout layout) {
    switch (layout) {
    case gungnir::ge::DisplayLayout::Fit: return "fit";
    case gungnir::ge::DisplayLayout::Stretch: return "stretch";
    case gungnir::ge::DisplayLayout::Crop: return "crop";
    }
    return "unknown";
}

// Interpolation state is file-scope for two reasons: main() has no stack room
// for it (the added locals overflowed the SDL thread stack once), and the menu
// paths must be able to drop the history when a session restarts.
struct InterpolationState {
    gungnir::ge::PresentSchedule schedule;
    gungnir::ge::FrameHistory history;
    std::vector<gungnir::ge::Draw> current;
    std::vector<gungnir::ge::Draw> scratch;
    // Per-pass counts, accumulated over a log tick: how many draws found a
    // partner, how many were paired by position instead of arrival order, and
    // how many buckets were too large to scan.
    gungnir::ge::InterpolationStats stats;
    std::uint64_t stat_passes{};
    gungnir::ge::InterpolationStats stat_total{};
    // Presentation timeline: which point in the guest's motion each present
    // shows, as guest frame index + blend factor. It must never go backwards;
    // a regression is the scene being shown earlier than it already was.
    std::uint64_t frames_shown{};
    double last_content{};
    bool has_content{};
    std::uint64_t timeline_presents{};
    std::uint64_t timeline_regressions{};
    std::uint64_t presents_since_frame{};
    std::uint64_t presents_min{};
    std::uint64_t presents_max{};
    // Guest frames whose scene did not move, so no in-between window was opened.
    // Counted so the power optimisation is visible rather than silent.
    std::uint64_t static_frames{};
};
InterpolationState g_interpolation;

// "Hold the last image" power optimisation. A guest frame that is EXACTLY
// identical to the one we last presented needs no new swapchain submit: on this
// device (30c1f958, VK_PRESENT_MODE_FIFO_KHR) SurfaceFlinger retains the last
// presented buffer for at least ~53 s without a new present (device experiment,
// build/qa/battery/hold_*.png). Exact identity means holding it can never
// change the picture. A periodic re-present (PPSSPP's 10-vblank fallback idea)
// keeps the layer alive and bounds any state we did not compare.
struct HeldImageState {
    std::vector<gungnir::ge::Draw> last_presented;
    bool has_last{};
    std::uint64_t consecutive_holds{};
    std::uint64_t presents_skipped{};
};
HeldImageState g_held_image;
inline constexpr std::uint64_t kHoldRePresentFrames = 10u;

// Widescreen coverage (9826wejc): the measured gap between what the widened
// frame shows and what the guest actually submitted. Cheap, bounds-only, done
// once per present; accumulated over a log tick. A meaningful step in the
// percentage (a scene change) gets a one-shot line so regressions are visible
// in a bounded run without waiting for the next tick.
struct WidescreenCoverageState {
    gungnir::ge::CoverageResult last;
    std::uint64_t samples{};
    std::uint64_t percent_sum{};
    std::uint64_t backdrop_sum{};
    int last_logged_percent{-1000};
    bool has_value{};
};
WidescreenCoverageState g_coverage;

// Widescreen coverage is QA instrumentation, not a render requirement: it scans
// every draw and fills a column bitmap on every present. Running it by default
// in the production render loop is a continuous, avoidable load. Opt in with the
// `coverage` intent extra (`--es coverage 1`); off by default.
bool g_coverage_enabled = false;

// A snapshot load, a reset to title or a background round trip makes the
// recorded frames a different continuity; never interpolate across it.
void reset_interpolation_history() {
    g_interpolation.schedule.reset();
    g_interpolation.history.clear();
    g_interpolation.current.clear();
    g_interpolation.scratch.clear();
    g_interpolation.stats = {};
    g_interpolation.stat_passes = 0u;
    g_interpolation.stat_total = {};
    // A new continuity must never hold an image from the old one: the next
    // frame is always presented, then a fresh last_presented is recorded.
    g_held_image.has_last = false;
    g_held_image.consecutive_holds = 0u;
}

// Bounded draw-list dump for offline presentation work. Host-side only, off
// unless `--es dump_draws <path>` names a file; it writes the first frames
// verbatim (key, through flag, and each vertex's x, y, z and texture
// coordinates) so an in-between can be reconstructed on the host instead of
// inferred from a video. Nothing here affects what is presented.
struct DrawDump {
    std::FILE *file{};
    std::uint32_t frames_written{};
    std::uint32_t limit{8u};
    std::int64_t delay_ms{};
    gungnir::ge::FrameQueue::Clock::time_point opened{};
};
DrawDump g_draw_dump;
std::string g_draw_dump_path;
// The folding artifact is visible in the presented in-between but appears in
// neither guest endpoint frame, so a second bounded dump captures to_present.
DrawDump g_present_dump;
std::string g_present_dump_path;

void write_dump_frame(DrawDump &dump, const std::string &path,
                      const std::vector<gungnir::ge::Draw> &draws, const char *tag) {
    if (dump.file == nullptr || dump.frames_written >= dump.limit)
        return;
    if (dump.delay_ms > 0) {
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            gungnir::ge::FrameQueue::Clock::now() - dump.opened);
        if (waited.count() < dump.delay_ms) return;
    }
    const auto write_u32 = [](std::uint32_t value, std::FILE *file) {
        std::fwrite(&value, sizeof value, 1u, file);
    };
    const auto write_f32 = [](float value, std::FILE *file) {
        std::fwrite(&value, sizeof value, 1u, file);
    };
    write_u32(static_cast<std::uint32_t>(draws.size()), dump.file);
    for (const gungnir::ge::Draw &draw : draws) {
        const std::uint64_t key = gungnir::ge::draw_key(draw);
        const std::uint8_t through = draw.state.through ? 1u : 0u;
        const std::uint32_t count = static_cast<std::uint32_t>(draw.vertices.size());
        std::fwrite(&key, sizeof key, 1u, dump.file);
        std::fwrite(&through, sizeof through, 1u, dump.file);
        write_u32(count, dump.file);
        for (const gungnir::ge::ScreenVertex &vertex : draw.vertices) {
            write_f32(vertex.x, dump.file);
            write_f32(vertex.y, dump.file);
            write_f32(vertex.z, dump.file);
            write_f32(vertex.texcoord[0], dump.file);
            write_f32(vertex.texcoord[1], dump.file);
        }
    }
    ++dump.frames_written;
    if (dump.frames_written == dump.limit) {
        std::fflush(dump.file);
        if (tag == std::string_view{"present_dump"}) {
            GUNGNIR_LOGI("present_dump frames=%u draws_last=%zu path=%s\n",
                         dump.frames_written, draws.size(), path.c_str());
        } else {
            GUNGNIR_LOGI("draw_dump frames=%u draws_last=%zu path=%s\n",
                         dump.frames_written, draws.size(), path.c_str());
        }
    }
}

void dump_guest_frame(const std::vector<gungnir::ge::Draw> &draws) {
    write_dump_frame(g_draw_dump, g_draw_dump_path, draws, "draw_dump");
}

void dump_present_frame(const std::vector<gungnir::ge::Draw> &draws) {
    if (g_present_dump.file == nullptr ||
        g_present_dump.frames_written >= g_present_dump.limit)
        return;
    write_dump_frame(g_present_dump, g_present_dump_path, draws, "present_dump");
}

std::atomic<std::int64_t> g_due_delta_min_us{0};
std::atomic<std::int64_t> g_due_delta_max_us{0};

void publish_guest_frame(gungnir::ge::FrameQueue &queue,
    const std::vector<gungnir::ge::Draw> &draws) {
    const auto due = frame_due_at();
    const auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        due - gungnir::ge::FrameQueue::Clock::now()).count();
    std::int64_t seen = g_due_delta_min_us.load();
    while (delta < seen && !g_due_delta_min_us.compare_exchange_weak(seen, delta)) {}
    seen = g_due_delta_max_us.load();
    while (delta > seen && !g_due_delta_max_us.compare_exchange_weak(seen, delta)) {}
    queue.publish(draws, due);
}

// One display frame at the capped 60 Hz: the maximum time the main loop may
// sleep without polling SDL, so touch and the overlay stay responsive. Input
// is pumped ONLY by this loop's SDL_PollEvent (there is no separate pump
// thread), so a longer sleep would add that much touch latency. This is a
// measured tradeoff, not a free win: PPSSPP's WaitUntil() can wait to the real
// deadline because its input rides the emulator's own vblank loop.
inline constexpr auto kIdleResponsivenessCeiling = std::chrono::milliseconds(16);

// Wait to the actual next deadline (an interpolation tick, or the next guest
// frame), bounded by the responsiveness ceiling. Publishing wakes the wait
// early via the queue's condition variable, so a guest frame is still
// presented within a millisecond of its deadline; the old fixed 8 ms slice
// woke the loop twice for a 16 ms tick. Behavioural reference only.
void sleep_for_next_frame(gungnir::ge::FrameQueue &queue,
    std::optional<gungnir::ge::FrameQueue::Clock::time_point> deadline = std::nullopt) {
    const auto slice = gungnir::ge::idle_wait_slice(
        gungnir::ge::FrameQueue::Clock::now(), deadline, kIdleResponsivenessCeiling);
    queue.wait_for_next(slice);
}

void log_stack_space(const char *thread) {
    const gungnir::platform::StackSpace space = gungnir::platform::current_stack_space();
    if (!space.known) {
        GUNGNIR_LOGE("stack_space thread=%s unavailable\n", thread);
        return;
    }
    GUNGNIR_LOGI("stack_space thread=%s limit_kb=%zu used_kb=%zu headroom_kb=%zu\n", thread,
                 space.limit_bytes / 1024u, space.used_bytes / 1024u,
                 space.headroom_bytes / 1024u);
}

void run_session_worker(gungnir::session::GameSession &session, std::atomic<bool> &finished) {
    // The guest runs on this thread, so its stack is the budget every HLE and
    // recompiled frame shares. Report it once instead of assuming a size.
    log_stack_space("session");
    session.run();
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

// The foreground half of the lifecycle policy. The guest only comes back once
// the new surface exists, so its first frame has somewhere to land, and the
// relaunch shares the snapshot mutex with the menu ops that join the same
// worker.
void resume_after_background(gungnir::session::GameSession &session,
    std::atomic<bool> &finished, std::thread &slot,
    gungnir::session::SessionLifecycle &lifecycle) {
    if (!lifecycle.paused()) return;
    std::lock_guard<std::mutex> lock(g_snapshot_mutex);
    // The worker reports its result as it exits; reading it while the worker
    // still runs would be a race, so the pause stays pending until it returns.
    if (!finished.load()) return;
    const bool stopped_by_host =
        session.result().stop_cause == gungnir::session::StopCause::HostStop;
    if (lifecycle.on_foreground(true, stopped_by_host) !=
        gungnir::session::SessionLifecycle::Action::Resume) {
        // A run that ended for its own reason (dispatch or wall-clock bound,
        // a failure) is not revived, but never silently.
        GUNGNIR_LOGI("session_resume_skipped reason=own_stop cause=%s\n",
                     session.result().stop_reason.c_str());
        return;
    }
    if (slot.joinable()) slot.join();
    reset_interpolation_history();
    session.request_resume();
    launch_session_thread(session, finished, slot);
    GUNGNIR_LOGI("session_resumed reason=foreground\n");
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

// Load the optional per-title host profile from the app-private files dir.
// A missing file is the normal case and changes nothing. A present-but-invalid
// file is a named error (logged and refused): no entry is applied. Every
// applied entry is logged as 'profile key=value' so a device boot shows
// exactly which host patches are active.
void load_host_profile(const std::filesystem::path &internal) {
    if (internal.empty()) {
        GUNGNIR_LOGI("profile absent\n");
        return;
    }
    const std::filesystem::path path =
        internal / "profiles" / "gungnir" / "local" / "host_patches.toml";
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        GUNGNIR_LOGI("profile absent\n");
        return;
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    const gungnir::host::ProfileResult result =
        gungnir::host::HostProfile::parse(text);
    if (!result.ok()) {
        GUNGNIR_LOGE("profile_invalid: %s\n", result.message.c_str());
        return;
    }
    g_host_profile = result.profile;
    for (const std::string &line : g_host_profile.applied_lines())
        GUNGNIR_LOGI("%s\n", line.c_str());
    GUNGNIR_LOGI("profile applied=%zu\n", g_host_profile.size());
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
    if (g_coverage_enabled) {
        const gungnir::ge::CoverageResult coverage =
            gungnir::ge::measure_widescreen_coverage(
                draws, static_cast<float>(g_widescreen_scale));
        g_coverage.last = coverage;
        g_coverage.has_value = true;
        ++g_coverage.samples;
        g_coverage.percent_sum += static_cast<std::uint64_t>(coverage.coverage_percent);
        g_coverage.backdrop_sum += static_cast<std::uint64_t>(coverage.backdrop_only_columns);
        // A 10-point step is a scene change, not frame-to-frame noise. One shot
        // per change, never once per present.
        if (std::abs(coverage.coverage_percent - g_coverage.last_logged_percent) >= 10) {
            GUNGNIR_LOGI("widescreen coverage=%d%% backdrop_only_columns=%d hook=absent\n",
                         coverage.coverage_percent, coverage.backdrop_only_columns);
            g_coverage.last_logged_percent = coverage.coverage_percent;
        }
    }
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
        out.through = in.through;
        // Full-frame 2D effects (backdrop, fades) stretch with the widened
        // scene; HUD keeps its pixel size and is only recentred.
        const bool fullscreen_2d = in.through && g_widescreen_scale > 1.0 &&
            gungnir::ge::covers_guest_frame(source.vertices, 480, 272);
        out.fullscreen_2d = fullscreen_2d;
        if (in.through && g_widescreen_scale > 1.0) {
            for (std::size_t v = 0; v < owned[i].vertices.size(); ++v)
                owned[i].vertices[v].x = gungnir::ge::wide_screen_x(
                    owned[i].vertices[v].x, static_cast<float>(g_widescreen_scale), fullscreen_2d);
        }
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
    gungnir::session::SessionLifecycle *lifecycle{};
    // Null when no worker was launched (debug quad), so a background event in
    // that mode never reports a live session or requests a stop.
    std::atomic<bool> *session_finished{};
    gungnir::audio::AAudioOut *audio_out{};
    gungnir::audio::MixerSink *mixer{};
};

bool lifecycle_watch(void *userdata, SDL_Event *event) {
    auto *state = static_cast<LifecycleState *>(userdata);
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND) {
        if (state->sdl_input != nullptr) state->sdl_input->release_held();
        if (state->gamepad != nullptr) state->gamepad->release_all();
        gungnir::input::state().release_all();
        // A held trigger must not survive the pause; a latched fast-forward is
        // the user's choice and does.
        gungnir::input::host_controls().release_holds();
        state->background.store(true);
        // The guest is stopped between dispatches while the OS owns the
        // surface: there is nothing to present to, and a game left running
        // behind another app only burns battery. Foreground relaunches it from
        // the pc it stopped at, so a round trip keeps progress without a
        // restart. A finished run (bounded QA, dispatch limit, failure) is not
        // a live session and is never revived.
        const bool session_live =
            state->session_finished != nullptr && !state->session_finished->load();
        if (state->lifecycle != nullptr && state->session != nullptr &&
            state->lifecycle->on_background(session_live) ==
                gungnir::session::SessionLifecycle::Action::Stop) {
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
        // The paused stream drains nothing, so the guest's last submissions
        // before it stops would fill the ring and count overruns. SDL blocks
        // the event pump while backgrounded, so the main loop cannot do this:
        // the mute has to happen here, with the stop.
        if (state->mixer != nullptr) {
            state->mixer->set_muted(true);
            GUNGNIR_LOGI("audio_muted reason=background\n");
        }
        GUNGNIR_LOGI("lifecycle_paused\n");
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (state->sdl_input != nullptr) state->sdl_input->release_held();
        if (state->gamepad != nullptr) state->gamepad->release_all();
        gungnir::input::state().release_all();
        gungnir::input::host_controls().release_holds();
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
        // A latched fast-forward stays muted; the main loop keeps ownership
        // of the control from here (it sees the same value and does nothing).
        if (state->mixer != nullptr &&
            !gungnir::input::host_controls().active(gungnir::input::HostControl::FastForward)) {
            state->mixer->set_muted(false);
            GUNGNIR_LOGI("audio_muted reason=foreground\n");
        }
        GUNGNIR_LOGI("lifecycle_event foreground\n");
    }
    return true;
}

} // namespace

#if defined(__ANDROID__)
struct SessionStartup {
    std::string internal; // empty when SDL reported no internal path
    gungnir::audio::AudioSink *audio_sink = nullptr;
    gungnir::ge::FrameQueue *frame_queue = nullptr;
    gungnir::session::GameSession *session = nullptr;
    std::thread *session_thread = nullptr;
    std::atomic<bool> *session_finished = nullptr;
    bool *session_launched = nullptr;
};

SessionStartup *g_startup_context = nullptr;

// On-device installer (Yakumo-style): if the verified ISO is present but no
// decrypted game.elf is on disk yet, prepare it from the ISO's EBOOT.BIN.
// This is what makes "pick an ISO and the game should start" true for the
// empty-corpus build; the old two-step flow returned VerifiedButNoGameCode
// and never started.
std::filesystem::path ensure_runtime_elf(const gungnir::platform::GameDataResult &game_data,
    const std::vector<std::filesystem::path> &game_directories) {
    std::filesystem::path elf = find_runtime_elf(game_directories);
    if (!elf.empty()) return elf;
    const std::filesystem::path data_dir = game_data.path.parent_path();
    GUNGNIR_LOGI("installer_prepare iso=%s\n", game_data.path.string().c_str());
    try {
        elf = gungnir::platform::install_from_iso(game_data.path, data_dir);
    } catch (const gungnir::platform::InstallError &error) {
        GUNGNIR_LOGE("installer_failed: %s\n", error.what());
        return {};
    }
    GUNGNIR_LOGI("installer_prepared elf=%s\n", elf.string().c_str());
    return elf;
}

bool try_start_session(SessionStartup &ctx) {
    if (ctx.session_launched != nullptr && *ctx.session_launched) return true;
    if (ctx.internal.empty() || ctx.audio_sink == nullptr || ctx.frame_queue == nullptr ||
        ctx.session == nullptr || ctx.session_thread == nullptr ||
        ctx.session_finished == nullptr || ctx.session_launched == nullptr) {
        GUNGNIR_LOGE("session_start_failed: startup_not_ready\n");
        return false;
    }
    gungnir::platform::GameDataResult game_data = check_game_data();
    if (!game_data.ok) {
        GUNGNIR_LOGE("session_start_failed: game_data_unavailable\n");
        return false;
    }
    std::vector<std::filesystem::path> game_directories;
    game_directories.emplace_back(ctx.internal);
    if (const char *external = SDL_GetAndroidExternalStoragePath(); external != nullptr)
        game_directories.emplace_back(external);
    const std::filesystem::path elf = ensure_runtime_elf(game_data, game_directories);
    if (elf.empty()) {
        GUNGNIR_LOGE("session_start_failed: game_elf_missing (installer could not prepare game.elf from the ISO; see installer_failed)\n");
        return false;
    }
    gungnir::session::Config session_config;
    session_config.elf_path = elf;
    session_config.nids_path = std::filesystem::path(ctx.internal) / "runtime" / "nids.csv";
    session_config.disc_path = game_data.path;
    session_config.memory_stick = std::filesystem::path(ctx.internal) / "psp";
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
    ctx.session->set_diagnostic_sink([](std::string_view line) {
        GUNGNIR_LOGE("%.*s\n", static_cast<int>(line.size()), line.data());
    });
    std::string start_error;
    if (!ctx.session->start(session_config, ctx.audio_sink, start_error)) {
        GUNGNIR_LOGE("session_start_failed: %s\n", start_error.c_str());
        return false;
    }
    GUNGNIR_LOGI("session_start elf=%s\n", elf.string().c_str());
    gungnir::ge::FrameQueue *queue = ctx.frame_queue;
    gungnir::hle::set_ge_frame_sink([queue](const std::vector<gungnir::ge::Draw> &draws) {
        publish_guest_frame(*queue, draws);
    });
    {
        g_snapshot_session = ctx.session;
        g_snapshot_thread = ctx.session_thread;
        g_snapshot_finished = ctx.session_finished;
        g_session_rearm = [queue] {
            gungnir::hle::set_ge_frame_sink(
                [queue](const std::vector<gungnir::ge::Draw> &draws) {
                    publish_guest_frame(*queue, draws);
                });
        };
    }
    const std::string snapshot_in = intent_string("snapshot_in");
    if (!snapshot_in.empty()) {
        std::string snapshot_error;
        if (!ctx.session->load_snapshot(snapshot_in, snapshot_error)) {
            GUNGNIR_LOGE("session_snapshot_in_failed: %s\n", snapshot_error.c_str());
        } else {
            GUNGNIR_LOGI("session_snapshot_in=%s\n", snapshot_in.c_str());
        }
    }
    launch_session_thread(*ctx.session, *ctx.session_finished, *ctx.session_thread);
    *ctx.session_launched = true;
    return true;
}

// Import result codes for Java: 0 rejected, 1 verified + prepared
// (session running or starting), 2 verified but the on-device installer
// could not prepare game.elf, 3 verified + prepared but the session
// failed to start (see logcat).
extern "C" JNIEXPORT jint JNICALL
Java_com_gungnir_recomp_GungnirActivity_nativeGameDataImported(JNIEnv *env, jobject, jstring path) {
    if (path == nullptr) return 0;
    const char *utf = env->GetStringUTFChars(path, nullptr);
    if (utf == nullptr) return 0;
    const std::string selected(utf);
    env->ReleaseStringUTFChars(path, utf);
    gungnir::platform::GameDataResult result =
        gungnir::platform::verify_game_data(selected, gungnir::platform::kGungnirUsaIdentity);
    result.origin = "saf";
    if (!result.ok) {
        GUNGNIR_LOGE("game_data_invalid: %s\n", result.error.c_str());
        return 0;
    }
    GUNGNIR_LOGI("game_data_ok origin=saf size=%llu crc=%08X\n",
                 static_cast<unsigned long long>(result.size),
                 static_cast<unsigned>(result.crc32));
    if (g_startup_context == nullptr) {
        // The picker needs a running activity, so this is unreachable in
        // practice; the verified ISO stays on disk and the next launch
        // boots it. Named, never silent.
        GUNGNIR_LOGE("game_data_import_deferred: startup_not_ready\n");
        return 1;
    }
    std::vector<std::filesystem::path> game_directories;
    if (!g_startup_context->internal.empty())
        game_directories.emplace_back(g_startup_context->internal);
    if (const char *external = SDL_GetAndroidExternalStoragePath(); external != nullptr)
        game_directories.emplace_back(external);
    // Yakumo-style: the SAF copy already landed as imported.iso; prepare
    // game.elf from it on-device, then start. An installer failure is a
    // distinct named outcome (code 2), never the old "add game.elf" dead end.
    try {
        const std::filesystem::path data_dir = std::filesystem::path(selected).parent_path();
        (void)gungnir::platform::install_from_iso(selected, data_dir);
        GUNGNIR_LOGI("installer_prepared elf=%s\n", (data_dir / "game.elf").string().c_str());
    } catch (const gungnir::platform::InstallError &error) {
        GUNGNIR_LOGE("installer_failed: %s\n", error.what());
        return 2;
    }
    {
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        if (try_start_session(*g_startup_context)) return 1;
    }
    return 3;
}

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_HostBridge_nativeSetFrameInterpolation(JNIEnv *, jclass, jboolean enabled) {
    g_interpolation_enabled.store(enabled == JNI_TRUE);
    GUNGNIR_LOGI("frame_interpolation active=%d\n", enabled == JNI_TRUE ? 1 : 0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_gungnir_recomp_input_HostBridge_frameInterpolation(JNIEnv *, jclass) {
    return g_interpolation_enabled.load() ? JNI_TRUE : JNI_FALSE;
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
    if (!g_snapshot_session->save_snapshot(file, error)) {
        // Never strand a stopped session: resume and relaunch so the game
        // stays alive, and log the cause (the toast alone is not evidence).
        GUNGNIR_LOGE("snapshot_save_failed: %s\n", error.c_str());
        g_snapshot_session->request_resume();
        launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
        g_lifecycle_reset_requested.store(true);
        return env->NewStringUTF(error.c_str());
    }
    GUNGNIR_LOGI("snapshot_saved path=%s\n", file.c_str());
    g_snapshot_session->request_resume();
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    g_lifecycle_reset_requested.store(true);
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
    reset_interpolation_history();
    if (!g_snapshot_session->load_snapshot(file, error)) {
        // Never strand a stopped session: the relaunched worker reports the
        // recorded load error loudly instead of freezing on a dead frame.
        GUNGNIR_LOGE("snapshot_load_failed: %s\n", error.c_str());
        launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
        g_lifecycle_reset_requested.store(true);
        return env->NewStringUTF(error.c_str());
    }
    GUNGNIR_LOGI("snapshot_loaded path=%s\n", file.c_str());
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    g_lifecycle_reset_requested.store(true);
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
    reset_interpolation_history();
    std::string error;
    if (!g_snapshot_session->restart_to_title(error)) {
        GUNGNIR_LOGE("reset_to_title_failed: %s\n", error.c_str());
        launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
        g_lifecycle_reset_requested.store(true);
        return env->NewStringUTF(error.c_str());
    }
    if (g_session_rearm) g_session_rearm();
    GUNGNIR_LOGI("session_reset_to_title\n");
    launch_session_thread(*g_snapshot_session, *g_snapshot_finished, *g_snapshot_thread);
    g_lifecycle_reset_requested.store(true);
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

extern "C" JNIEXPORT void JNICALL
Java_com_gungnir_recomp_input_HostBridge_nativeSetDisplayLayout(JNIEnv *env, jclass, jstring policy) {
    const std::string text = jstring_to_string(env, policy);
    std::string error;
    const gungnir::ge::DisplayLayout layout = gungnir::ge::parse_display_layout(text, &error);
    if (!error.empty()) {
        // Named, not silent: the layout stays as it was.
        GUNGNIR_LOGE("%s\n", error.c_str());
        return;
    }
    g_display_layout.store(layout);
    GUNGNIR_LOGI("display_layout=%s\n", display_layout_name(layout));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_gungnir_recomp_input_HostBridge_displayLayout(JNIEnv *env, jclass) {
    return env->NewStringUTF(display_layout_name(g_display_layout.load()));
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
Java_com_gungnir_recomp_input_HostBridge_fastForwardActive(JNIEnv *, jclass) {
    return gungnir::input::host_controls().active(gungnir::input::HostControl::FastForward)
        ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_gungnir_recomp_input_HostBridge_fastForwardSpeed(JNIEnv *, jclass) {
    return static_cast<jfloat>(gungnir::input::kFastForwardSpeed);
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

// Late session start (yt4rpgt2): a verified SAF import can land after
// startup already passed its launch branch, so the launch sequence must be
// callable again. The caller holds g_snapshot_mutex: the same mutex the
// menu ops join the worker under, so a concurrent save/load/reset can
// never meet a half-started session. Returns true when the session thread
// is running afterwards; a second call with a live session is a no-op
// true; without game data or an ELF it logs the named reason.
int main(int, char **) {
    log_stack_space("main");
    // Touch input belongs to the TouchOverlayView (JNI); synthetic mouse
    // events from touches would reach SdlInput and map to Cross/Circle/
    // Square through the desktop mouse bindings — a phantom press on every
    // tap that falls through to the SDL surface (e.g. overlay hidden by a
    // gamepad). Real mouse input still works; only the synthesis is off.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        GUNGNIR_LOGE("sdl_init_failed: %s\n", SDL_GetError());
        return 1;
    }
    const char *internal = SDL_GetAndroidInternalStoragePath();
    if (internal != nullptr) unpack_runtime_assets(internal);
    load_host_profile(internal != nullptr ? std::filesystem::path(internal)
                                          : std::filesystem::path{});
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
    {
        // QA form: --es display_layout fit|stretch|crop. An unknown name is a
        // named error and the default (fit) stands.
        const std::string requested = intent_string("display_layout");
        if (!requested.empty()) {
            std::string layout_error;
            const gungnir::ge::DisplayLayout layout =
                gungnir::ge::parse_display_layout(requested, &layout_error);
            if (!layout_error.empty()) {
                GUNGNIR_LOGE("%s\n", layout_error.c_str());
            } else {
                g_display_layout.store(layout);
                config.display_layout = layout;
            }
        }
    }
    // Widescreen widening: the perspective scene is rendered into a frame as
    // wide as the display needs, so the picture fills it with no bars, no
    // stretching and nothing cut. Screen-space (through) draws keep their
    // 480-wide mapping and are only recentred. "off" is the native 480x272
    // frame; "16:9" caps the widening at the guest's own 16:9-ish aspect.
    {
        g_coverage_enabled = intent_long("coverage", 0) != 0;
        if (g_coverage_enabled)
            GUNGNIR_LOGI("coverage opt-in enabled\n");
        g_draw_dump_path = intent_string("dump_draws");
        if (!g_draw_dump_path.empty()) {
            g_draw_dump.delay_ms = intent_long("dump_after_ms", 0);
            const std::int64_t frames = intent_long("dump_frames", 8);
            g_draw_dump.limit = frames > 0 && frames <= 32 ? static_cast<std::uint32_t>(frames) : 8u;
            g_draw_dump.opened = gungnir::ge::FrameQueue::Clock::now();
            g_draw_dump.file = std::fopen(g_draw_dump_path.c_str(), "wb");
            if (g_draw_dump.file == nullptr)
                GUNGNIR_LOGE("draw_dump_failed: %s\n", g_draw_dump_path.c_str());
            else
                GUNGNIR_LOGI("draw_dump path=%s frames=%u delay_ms=%lld\n",
                             g_draw_dump_path.c_str(), g_draw_dump.limit,
                             static_cast<long long>(g_draw_dump.delay_ms));
        }
        g_present_dump_path = intent_string("dump_presents");
        if (!g_present_dump_path.empty()) {
            g_present_dump.delay_ms = g_draw_dump.delay_ms;
            g_present_dump.limit = g_draw_dump.limit;
            g_present_dump.opened = g_draw_dump.opened;
            g_present_dump.file = std::fopen(g_present_dump_path.c_str(), "wb");
            if (g_present_dump.file == nullptr)
                GUNGNIR_LOGE("present_dump_failed: %s\n", g_present_dump_path.c_str());
            else
                GUNGNIR_LOGI("present_dump path=%s frames=%u delay_ms=%lld\n",
                             g_present_dump_path.c_str(), g_present_dump.limit,
                             static_cast<long long>(g_present_dump.delay_ms));
        }
        const std::string mode = intent_string("widescreen");
        // adaptive (default) follows the window; the aspect caps bound it.
        const std::string policy = mode.empty() ? "adaptive" : mode;
        double target_aspect = 0.0;
        int window_width = 0;
        int window_height = 0;
        if (SDL_GetWindowSizeInPixels(window, &window_width, &window_height) && window_height > 0)
            target_aspect = static_cast<double>(window_width) / window_height;
        constexpr double kGuestAspect = 480.0 / 272.0;
        double cap = 0.0;
        if (policy == "off") {
            cap = 0.0;
        } else if (policy == "adaptive" || policy == "display") {
            cap = target_aspect;
        } else if (policy == "16:9") {
            cap = 16.0 / 9.0;
        } else if (policy == "21:9") {
            cap = 21.0 / 9.0;
        } else if (policy == "32:9") {
            cap = 32.0 / 9.0;
        } else {
            GUNGNIR_LOGE("unknown_widescreen: %s\n", policy.c_str());
            cap = 0.0;
        }
        const double scale = cap > kGuestAspect ? cap / kGuestAspect : 1.0;
        g_widescreen_scale = scale > 1.0 ? scale : 1.0;
        config.virtual_width = static_cast<int>(std::lround(480.0 * g_widescreen_scale));
        gungnir::hle::set_widescreen_scale(static_cast<float>(g_widescreen_scale));
        GUNGNIR_LOGI("widescreen mode=%s scale=%.4f virtual=%dx%d\n", policy.c_str(),
                     g_widescreen_scale, config.virtual_width, config.virtual_height);
    }
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
    GUNGNIR_LOGI("display_layout=%s\n", display_layout_name(config.display_layout));
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
    if (intent_long("fast_forward", 0) != 0) {
        // QA form: start latched so a bounded run can measure 3x progress.
        gungnir::input::host_controls().latch(
            gungnir::input::HostControl::FastForward, true);
        GUNGNIR_LOGI("fast_forward intent=on\n");
    }
    gungnir::ge::FrameQueue frame_queue;
    gungnir::session::GameSession session;
    std::thread session_thread;
    std::atomic<bool> session_finished{false};
    bool session_launched = false;
    // The startup context outlives the if block: JNI late imports (SAF pick
    // after launch) dereference g_startup_context seconds later, so it must
    // not be a block-local that dangles (startup_not_ready on first pick).
    SessionStartup startup;
    if (debug_quad) {
        GUNGNIR_LOGI("debug_quad enabled\n");
    } else {
        startup.internal = internal != nullptr ? internal : "";
        startup.audio_sink = audio_sink;
        startup.frame_queue = &frame_queue;
        startup.session = &session;
        startup.session_thread = &session_thread;
        startup.session_finished = &session_finished;
        startup.session_launched = &session_launched;
        g_startup_context = &startup;
        // try_start_session re-checks game data and the ELF itself and
        // logs the named reason, so the dead-end branch it replaces
        // (silent when data was ok but internal was null) is covered.
        std::lock_guard<std::mutex> lock(g_snapshot_mutex);
        try_start_session(startup);
    }

    LifecycleState lifecycle;
    gungnir::session::SessionLifecycle lifecycle_policy;
    gungnir::input::SdlInput sdl_input;
    lifecycle.sdl_input = &sdl_input;
    lifecycle.gamepad = &gamepad;
    lifecycle.session = &session;
    lifecycle.lifecycle = &lifecycle_policy;
    lifecycle.session_finished = session_launched ? &session_finished : nullptr;
    lifecycle.audio_out = audio_out.get();
    lifecycle.mixer = &mixer;
    SDL_AddEventWatch(lifecycle_watch, &lifecycle);

    bool running = true;
    bool first_frame = true;
    bool fast_forward_active = false;
    bool mixer_muted = false;
    if (intent_long("frame_interpolation", 0) != 0) {
        g_interpolation_enabled.store(true);
        GUNGNIR_LOGI("frame_interpolation intent=on\n");
    }
    // The display's cadence bounds the in-betweens, but a high-refresh phone
    // must not drive 120+ recomputed presents per second of a 20 Hz game: the
    // host pacing policy caps the effective refresh at 60 Hz (PPSSPP's
    // FramerateTarget() does the same). Behavioural reference only.
    double refresh_hz = 60.0;
    if (const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        mode != nullptr && mode->refresh_rate > 1.0f) {
        refresh_hz = mode->refresh_rate;
    }
    const double capped_hz = gungnir::ge::cap_refresh_hz(refresh_hz);
    const auto refresh_interval = std::chrono::duration_cast<gungnir::ge::FrameQueue::Clock::duration>(
        std::chrono::duration<double>(1.0 / capped_hz));
    GUNGNIR_LOGI("display_refresh_hz=%.1f capped_hz=%.1f\n", refresh_hz, capped_hz);
    gungnir::ge::PresentSchedule &schedule = g_interpolation.schedule;
    schedule.set_refresh_interval(refresh_interval);
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

        // A menu save/load/reset relaunched the worker while the activity was
        // foreground: a pending background pause can never become a resume.
        if (g_lifecycle_reset_requested.exchange(false)) lifecycle_policy.reset();

        // Fast-forward is host pacing policy. Mute the mixer while it is on so
        // the guest's faster audio never overruns the device ring. The same
        // mute covers the background window: the stream is paused there, and
        // the few hundred milliseconds the guest needs to stop would otherwise
        // fill the ring and count overruns on every round trip.
        const bool fast_forward = gungnir::input::host_controls().active(
            gungnir::input::HostControl::FastForward);
        if (fast_forward != fast_forward_active) {
            fast_forward_active = fast_forward;
            GUNGNIR_LOGI("fast_forward active=%d speed=%.2f\n", fast_forward ? 1 : 0,
                         gungnir::input::host_controls().speed());
        }
        const bool mixer_mute = fast_forward || lifecycle.background.load();
        if (mixer_mute != mixer_muted) {
            mixer_muted = mixer_mute;
            mixer.set_muted(mixer_mute);
        }

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
                    // A fresh surface has no retained image to hold.
                    g_held_image.has_last = false;
                    g_held_image.consecutive_holds = 0u;
                    GUNGNIR_LOGI("lifecycle_resumed %dx%d\n", width, height);
                } else {
                    GUNGNIR_LOGE("surface_recreate_failed: %s\n", error.c_str());
                }
            }
            SDL_Delay(16);
            continue;
        }
        // The worker can still be inside an HLE call when the surface comes
        // back, so the resume is retried every frame the pause is pending
        // rather than only at the moment the surface was recreated.
        resume_after_background(session, session_finished, session_thread,
                                lifecycle_policy);

        std::vector<gungnir::ge::Draw> guest_draws;
        bool present_late = false;
        const auto frame_now = gungnir::ge::FrameQueue::Clock::now();
        const bool have_guest_frame = !debug_quad &&
            frame_queue.take_due(frame_now, guest_draws, present_late);
        if (present_late) {
            // The frame is still shown, one deadline late: name it rather than
            // hiding the miss under a longer sleep.
            static std::uint64_t late_lines = 0u;
            if (late_lines++ == 0u)
                GUNGNIR_LOGE("present_late: host behind the guest deadline by %lld us\n",
                             static_cast<long long>(frame_queue.last_late_us()));
        }
        bool rendered = false;
        std::chrono::microseconds present_time{};
        static std::chrono::microseconds present_total{};
        static std::uint64_t present_count{};
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
        } else {
            // Interpolation presents in-between frames, so it owns the decision
            // of what goes on screen; without it a guest frame is presented as
            // it arrives.
            std::vector<gungnir::ge::Draw> *to_present = nullptr;
            schedule.set_enabled(g_interpolation_enabled.load(std::memory_order_relaxed));
            backend.set_display_layout(g_display_layout.load(std::memory_order_relaxed));
            if (have_guest_frame) {
                const bool has_previous = !g_interpolation.current.empty();
                if (g_interpolation.timeline_presents != 0u) {
                    // Presents per guest frame: the display cadence the mode
                    // actually achieves, not the one it hopes for.
                    const std::uint64_t count = g_interpolation.presents_since_frame;
                    if (g_interpolation.presents_min == 0u || count < g_interpolation.presents_min)
                        g_interpolation.presents_min = count;
                    g_interpolation.presents_max = std::max(g_interpolation.presents_max, count);
                }
                g_interpolation.presents_since_frame = 0u;
                ++g_interpolation.frames_shown;
                // A scene that did not move between two guest frames has no
                // motion to blend. Skip the whole interpolation window so a
                // still picture is shown once at the guest rate instead of
                // driving the 120 Hz display with identical recomputed frames.
                const bool static_scene = has_previous &&
                    gungnir::ge::scene_is_static(guest_draws, g_interpolation.current);
                if (static_scene) ++g_interpolation.static_frames;
                const bool immediate = schedule.on_guest_frame(frame_now, has_previous,
                    static_scene);
                if (has_previous) g_interpolation.history.push(std::move(g_interpolation.current));
                g_interpolation.current = std::move(guest_draws);
                dump_guest_frame(g_interpolation.current);
                if (immediate) to_present = &g_interpolation.current;
            } else {
                const auto due = schedule.due(frame_now);
                const auto note_content = [&](double content) {
                    ++g_interpolation.timeline_presents;
                    ++g_interpolation.presents_since_frame;
                    if (g_interpolation.has_content && content + 1e-3 < g_interpolation.last_content) {
                        if (g_interpolation.timeline_regressions == 0u)
                            GUNGNIR_LOGE("timeline_regression content=%.3f last=%.3f\n",
                                         content, g_interpolation.last_content);
                        ++g_interpolation.timeline_regressions;
                    }
                    g_interpolation.last_content = content;
                    g_interpolation.has_content = true;
                };
                if (due.kind == gungnir::ge::PresentSchedule::Kind::Interpolated) {
                    g_interpolation.scratch = g_interpolation.current;
                    gungnir::ge::InterpolationStats stats;
                    gungnir::ge::interpolate_frame(g_interpolation.scratch,
                        g_interpolation.history, due.t, &stats);
                    g_interpolation.stats = stats;
                    g_interpolation.stat_total.candidates += stats.candidates;
                    g_interpolation.stat_total.matched += stats.matched;
                    g_interpolation.stat_total.cut += stats.cut;
                    g_interpolation.stat_total.unmatched += stats.unmatched;
                    g_interpolation.stat_total.reassigned += stats.reassigned;
                    g_interpolation.stat_total.displaced += stats.displaced;
                    g_interpolation.stat_total.largest_bucket = std::max(
                        g_interpolation.stat_total.largest_bucket, stats.largest_bucket);
                    g_interpolation.stat_total.oversize += stats.oversize;
                    g_interpolation.stat_total.oversize_draws += stats.oversize_draws;
                    g_interpolation.stat_total.translated += stats.translated;
                    g_interpolation.stat_total.frame_motion += stats.frame_motion;
                    ++g_interpolation.stat_passes;
                    note_content(static_cast<double>(g_interpolation.frames_shown - 1u) +
                                 static_cast<double>(due.t));
                    to_present = &g_interpolation.scratch;
                } else if (due.kind == gungnir::ge::PresentSchedule::Kind::Held) {
                    // The next guest frame is late: show the real one once.
                    note_content(static_cast<double>(g_interpolation.frames_shown - 1u) + 1.0);
                    to_present = &g_interpolation.current;
                }
            }
            if (to_present == nullptr) {
                // Nothing is due yet: wait precisely for the next interpolation
                // tick, or the next guest frame when no window is active,
                // bounded by the responsiveness ceiling.
                std::optional<gungnir::ge::FrameQueue::Clock::time_point> deadline;
                if (schedule.active())
                    deadline = schedule.next_tick();
                else
                    deadline = frame_queue.pending_deadline();
                sleep_for_next_frame(frame_queue, deadline);
                continue;
            }
            // Exact identity only: holding a byte-identical frame cannot change
            // the picture, and the device proved the compositor keeps the last
            // image. Never hold past the safety interval so an un-compared
            // state change or a lost layer is shown again promptly.
            const bool hold = g_held_image.has_last &&
                g_held_image.consecutive_holds < kHoldRePresentFrames &&
                gungnir::ge::frames_identical(*to_present, g_held_image.last_presented);
            if (hold) {
                ++g_held_image.consecutive_holds;
                ++g_held_image.presents_skipped;
                rendered = true;  // the swapchain still shows the last image
            } else {
                const auto present_started = gungnir::ge::FrameQueue::Clock::now();
                dump_present_frame(*to_present);
                rendered = present_guest_frame(backend, *to_present, error);
                present_time = std::chrono::duration_cast<std::chrono::microseconds>(
                    gungnir::ge::FrameQueue::Clock::now() - present_started);
                present_total += present_time;
                ++present_count;
                if (rendered) {
                    g_held_image.last_presented = *to_present;
                    g_held_image.has_last = true;
                    g_held_image.consecutive_holds = 0u;
                }
            }
        }
        if (!rendered) {
            GUNGNIR_LOGE("render_failed: %s\n", error.c_str());
            needs_surface = true;
            if (++render_failures >= 3) break;
            SDL_Delay(100);
            continue;
        }
        render_failures = 0;
        // Read the frame the interpolation state stored: `guest_draws` was moved
        // into it above, so testing or counting it here made this whole block
        // dead and the guest-frame counters silently never advanced.
        if (have_guest_frame && !g_interpolation.current.empty()) {
            const std::vector<gungnir::ge::Draw> &frame_draws = g_interpolation.current;
            ++guest_frames;
            std::size_t vertices = 0u;
            for (const gungnir::ge::Draw &draw : frame_draws) vertices += draw.vertices.size();
            if (guest_frames == 1u || guest_frames % 60u == 0u)
                GUNGNIR_LOGI("guest_frame=%llu draws=%zu vertices=%zu ge_gaps=%llu\n",
                             static_cast<unsigned long long>(guest_frames), frame_draws.size(), vertices,
                             static_cast<unsigned long long>(gungnir::hle::ge_gap_count()));
            if (guest_frames == 1u || guest_frames == 60u) {
                const auto &display = gungnir::hle::display_state();
                GUNGNIR_LOGI("guest_display=%ux%u fb=%08X width=%u format=%u\n", display.width,
                             display.height, display.framebuffer, display.buffer_width, display.pixel_format);
                const std::size_t shown = std::min<std::size_t>(frame_draws.size(), 12u);
                for (std::size_t i = 0; i < shown; ++i) {
                    const gungnir::ge::Draw &draw = frame_draws[i];
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
            GUNGNIR_LOGI("audio_mixer pulled=%llu underrun=%llu submitted=%llu muted=%llu overruns=%llu "
                         "rejected=%llu peak=%u xruns=%d\n",
                         static_cast<unsigned long long>(stats.frames_pulled),
                         static_cast<unsigned long long>(stats.underrun_frames),
                         static_cast<unsigned long long>(stats.submitted_buffers),
                         static_cast<unsigned long long>(stats.muted_buffers),
                         static_cast<unsigned long long>(stats.overruns),
                         static_cast<unsigned long long>(stats.rejected_buffers), stats.peak,
                         audio_out != nullptr ? audio_out->xruns() : 0);
            const gungnir::hle::CadenceCounters cadence = gungnir::hle::cadence_snapshot();
            GUNGNIR_LOGI("%s\n", gungnir::hle::format_cadence(cadence).c_str());
            GUNGNIR_LOGI("present_time frames=%llu total_us=%lld avg_us=%lld last_us=%lld\n",
                         static_cast<unsigned long long>(present_count),
                         static_cast<long long>(present_total.count()),
                         static_cast<long long>(present_count != 0u
                             ? present_total.count() / static_cast<std::int64_t>(present_count)
                             : 0),
                         static_cast<long long>(present_time.count()));
            GUNGNIR_LOGI("present_due min_us=%lld max_us=%lld last_late_us=%lld\n",
                         static_cast<long long>(g_due_delta_min_us.load()),
                         static_cast<long long>(g_due_delta_max_us.load()),
                         static_cast<long long>(frame_queue.last_late_us()));
            GUNGNIR_LOGI("interpolation active=%d interpolated=%llu held=%llu skipped_ticks=%llu static_frames=%llu presents_skipped=%llu\n",
                         g_interpolation_enabled.load() ? 1 : 0,
                         static_cast<unsigned long long>(schedule.interpolated()),
                         static_cast<unsigned long long>(schedule.held()),
                         static_cast<unsigned long long>(schedule.skipped()),
                         static_cast<unsigned long long>(g_interpolation.static_frames),
                         static_cast<unsigned long long>(g_held_image.presents_skipped));
            if (g_interpolation.stat_passes != 0u) {
                // Averages over the passes in this tick. reassigned is how many
                // partners came from position rather than arrival order -- the
                // draws arrival-order matching would have blended toward the
                // wrong object; oversize is the bucket scan limit firing.
                const auto passes = static_cast<unsigned long long>(g_interpolation.stat_passes);
                const auto &total = g_interpolation.stat_total;
                GUNGNIR_LOGI("interpolation_pairs passes=%llu candidates=%llu matched=%llu "
                             "cut=%llu unmatched=%llu reassigned=%llu displaced=%llu motion=%.1f "
                             "oversize=%llu oversize_draws=%llu translated=%llu largest_bucket=%llu\n",
                             passes,
                             static_cast<unsigned long long>(total.candidates) / passes,
                             static_cast<unsigned long long>(total.matched) / passes,
                             static_cast<unsigned long long>(total.cut) / passes,
                             static_cast<unsigned long long>(total.unmatched) / passes,
                             static_cast<unsigned long long>(total.reassigned) / passes,
                             static_cast<unsigned long long>(total.displaced) / passes,
                             static_cast<double>(total.frame_motion) / static_cast<double>(passes),
                             static_cast<unsigned long long>(total.oversize) / passes,
                             static_cast<unsigned long long>(total.oversize_draws) / passes,
                             static_cast<unsigned long long>(total.translated) / passes,
                             static_cast<unsigned long long>(total.largest_bucket));
                g_interpolation.stat_passes = 0u;
                g_interpolation.stat_total = {};
            }
            if (g_coverage_enabled && g_coverage.has_value && g_coverage.samples != 0u) {
                const auto samples = static_cast<unsigned long long>(g_coverage.samples);
                GUNGNIR_LOGI("widescreen coverage=%llu%% backdrop_only_columns=%llu hook=%s "
                             "samples=%llu scale=%.4f columns=%d\n",
                             static_cast<unsigned long long>(g_coverage.percent_sum / samples),
                             static_cast<unsigned long long>(g_coverage.backdrop_sum / samples),
                             "absent", samples, g_widescreen_scale,
                             g_coverage.last.total_columns);
                g_coverage.samples = 0u;
                g_coverage.percent_sum = 0u;
                g_coverage.backdrop_sum = 0u;
            }
            GUNGNIR_LOGI("timeline presents=%llu regressions=%llu frames=%llu per_frame=%llu..%llu "
                         "last_content=%.2f\n",
                         static_cast<unsigned long long>(g_interpolation.timeline_presents),
                         static_cast<unsigned long long>(g_interpolation.timeline_regressions),
                         static_cast<unsigned long long>(g_interpolation.frames_shown),
                         static_cast<unsigned long long>(g_interpolation.presents_min),
                         static_cast<unsigned long long>(g_interpolation.presents_max),
                         g_interpolation.last_content);
            GUNGNIR_LOGI("presents published=%llu presented=%llu dropped=%llu late=%llu\n",
                         static_cast<unsigned long long>(frame_queue.published()),
                         static_cast<unsigned long long>(frame_queue.presented()),
                         static_cast<unsigned long long>(frame_queue.dropped()),
                         static_cast<unsigned long long>(frame_queue.late()));
        }
        if (first_frame) {
            GUNGNIR_LOGI("first frame %dx%d\n", width, height);
            first_frame = false;
        }
        if (debug_quad)
            SDL_Delay(16);
        else
            sleep_for_next_frame(frame_queue, frame_queue.pending_deadline());
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

#include "session/game_session.hpp"

#include "hle/hle_media.hpp"

#include "session/diagnostics.hpp"

#include "hle/hle_atrac.hpp"
#include "hle/hle_audio.hpp"
#include "hle/hle_common.hpp"
#include "hle/hle_media.hpp"
#include "hle/hle_mpeg.hpp"
#include "hle/hle_module.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace gungnir::session {

using gungnir::hle::AllegrexContext;
using gungnir::hle::HleRegistrar;
using psprecomp::Elf32Image;
using psprecomp::PspImport;
using psprecomp::PspModuleInfo;
using psprecomp::Runtime;

struct GameSession::Impl {
    Config config;
    Result result;
    std::unique_ptr<Runtime> runtime;
    std::unique_ptr<HleRegistrar> hle;
    gungnir::audio::AudioSink *sink{};
    std::atomic<bool> stop_requested{false};
    bool started{};
    std::uint32_t entry{};
    std::uint32_t image_end{};
    std::uint32_t gp{};
    std::string module_name;
    std::uint64_t dispatches{};
    std::chrono::steady_clock::time_point started_at{};
    std::vector<std::string> unbound;
    std::size_t imports_total{};
    std::atomic<bool> watchdog_stop{false};
    std::thread watchdog;
    std::mutex diagnostic_mutex;
    std::function<void(std::string_view)> diagnostic_sink;

    void emit_diagnostic(const std::string &line) {
        std::function<void(std::string_view)> sink;
        {
            std::lock_guard lock(diagnostic_mutex);
            sink = diagnostic_sink;
        }
        if (sink) sink(line);
    }

    void fill_result() {
        result.dispatches = dispatches;
        if (runtime == nullptr) return;
        result.module = module_name;
        result.entry = entry;
        result.gp = gp;
        result.image_end = image_end;
        result.functions = runtime->function_count();
        result.dispatch_work = runtime->dispatch_work_count();
        result.vblanks = gungnir::hle::kernel().vblank_count();
        result.virtual_us = gungnir::hle::kernel().now_us();

        const std::string &reason = runtime->stop_reason();
        result.stop_reason = reason;
        result.stop_cause = classify_stop_cause(reason);
        result.reached_first_frame_buf = result.stop_cause == StopCause::FirstFlip;

        const auto &display = gungnir::hle::display_state();
        result.flips = display.flips;
        result.first_frame_buf = display.framebuffer;
        result.display_width = display.buffer_width;
        result.display_format = display.pixel_format;
        result.module_init_complete = display.flips > 0u && display.buffer_width != 0u &&
            result.stop_cause != StopCause::MissingImport;
        result.threads = gungnir::hle::kernel().describe_threads();
        result.hle_bound = hle != nullptr ? hle->count() : 0u;
        result.imports_total = imports_total;
        result.imports_bound = imports_total - unbound.size();
        result.imports_unbound = unbound;
        result.imports_executed.clear();
        for (const auto &[name, count] : runtime->hle_histogram()) {
            (void)count;
            result.imports_executed.push_back(name);
        }
        result.stopped = runtime->stopped();
        result.stop_pc = runtime->cpu().pc;
    }
};

namespace {

GameSession::Impl *g_active = nullptr;

void count_dispatch(Runtime &, AllegrexContext &, std::uint32_t, std::int32_t) {
    if (g_active != nullptr) ++g_active->dispatches;
}

void chain_pre(Runtime &, AllegrexContext &ctx, std::uint32_t target, std::uint32_t depth) {
    gungnir::session::chain_ring_enter(target, depth, ctx.gpr[31], ctx.gpr[4], ctx.gpr[5], ctx.gpr[6],
        ctx.gpr[7], psprecomp::runtime_thread_name());
}

void chain_post(Runtime &, AllegrexContext &ctx, std::uint32_t target, std::uint32_t depth) {
    gungnir::session::chain_ring_leave(target, depth, ctx.gpr[2], ctx.gpr[3]);
}

void heartbeat(std::uint64_t, std::uint32_t) {
    if (g_active == nullptr || g_active->runtime == nullptr) return;
    if (g_active->stop_requested.load(std::memory_order_relaxed)) {
        g_active->runtime->stop("host stop requested");
        return;
    }
    if (g_active->config.wall_seconds <= 0.0) return;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_active->started_at).count();
    if (elapsed >= g_active->config.wall_seconds)
        g_active->runtime->stop("wall-clock limit reached");
}

std::uint32_t image_end(const Elf32Image &elf, std::uint32_t load_base) {
    std::uint64_t end = 0u;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &segment = elf.segments()[i];
        if (segment.type != 1u) continue;
        end = std::max<std::uint64_t>(end, elf.segment_runtime_address(i, load_base) + segment.memory_size);
    }
    return static_cast<std::uint32_t>(end);
}

void clear_hooks() {
    psprecomp::set_runtime_heartbeat_hook(nullptr, 0u);
    psprecomp::set_runtime_pre_dispatch_hook(nullptr);
    psprecomp::set_runtime_pre_chained_call_hook(nullptr);
    psprecomp::set_runtime_post_chained_call_hook(nullptr);
}

} // namespace

GameSession::GameSession() : impl_(std::make_unique<Impl>()) {}

GameSession::~GameSession() {
    shutdown();
}

bool GameSession::start(const Config &config, gungnir::audio::AudioSink *sink, std::string &error) {
    shutdown();
    impl_->config = config;
    impl_->result = Result{};
    impl_->sink = sink;
    impl_->stop_requested.store(false, std::memory_order_relaxed);
    impl_->started = false;
    impl_->dispatches = 0u;
    impl_->unbound.clear();
    impl_->imports_total = 0u;

    if (!generated_corpus_present()) {
        error = "game_corpus_missing: rebuild with -Pgungnir.gameCorpus=DIR";
        return false;
    }
    if (!std::filesystem::is_regular_file(config.elf_path)) {
        error = "session_elf_missing: " + config.elf_path.string();
        return false;
    }
    if (!std::filesystem::is_regular_file(config.nids_path)) {
        error = "session_nids_missing: " + config.nids_path.string();
        return false;
    }
    if (sink == nullptr) {
        error = "session_audio_sink_missing";
        return false;
    }
    if (config.max_dispatches == 0u) {
        error = "session_dispatch_limit_zero";
        return false;
    }

    try {
        setenv("PSPRECOMP_HLE_HISTOGRAM", "1", 1);
        impl_->runtime = std::make_unique<Runtime>();
        gungnir::audio::set_audio_sink(sink);
        gungnir::hle::load_nid_registry(*impl_->runtime, config.nids_path);

        const Elf32Image elf = Elf32Image::from_file(config.elf_path);
        impl_->entry = elf.runtime_entry(config.load_base);
        (void)elf.load_and_relocate(impl_->runtime->memory(), config.load_base);
        const auto module = elf.find_module_info(impl_->runtime->memory(), config.load_base);
        if (!module) throw psprecomp::Error("PSP module info not found in " + config.elf_path.string());
        impl_->module_name = module->name;
        impl_->gp = module->gp;
        impl_->image_end = image_end(elf, config.load_base);

        psprecomp::register_generated_functions(*impl_->runtime);
        gungnir::hle::kernel() = gungnir::hle::Kernel{};
        gungnir::hle::kernel().install(*impl_->runtime, module->gp, impl_->image_end);
        gungnir::hle::kernel().set_pacing_enabled(config.pacing);
        gungnir::hle::reset_media_state();
        gungnir::hle::reset_modules();
        gungnir::hle::register_main_module(module->name, config.load_base,
            impl_->image_end - config.load_base, module->gp);

        impl_->hle = std::make_unique<HleRegistrar>(*impl_->runtime);
        gungnir::hle::register_sysmem(*impl_->hle);
        gungnir::hle::register_threadman(*impl_->hle);
        gungnir::hle::register_system(*impl_->hle);
        gungnir::hle::register_io(*impl_->hle, config.disc_path, config.memory_stick);
        gungnir::hle::register_media(*impl_->hle);
        gungnir::hle::register_mpeg(*impl_->hle);
        gungnir::hle::reset_audio_state();
        gungnir::hle::register_audio(*impl_->hle);
        gungnir::hle::reset_atrac_state();
        gungnir::hle::register_atrac(*impl_->hle);

        const auto imports = elf.scan_imports(impl_->runtime->memory(), *module);
        impl_->imports_total = imports.size();
        for (const PspImport &import : imports) {
            if (!impl_->hle->bound(import.library, import.nid))
                impl_->unbound.push_back(import.library + ":" + psprecomp::hex32(import.nid));
        }

        for (std::size_t i = 0; i < config.boot_path.size(); ++i)
            impl_->runtime->memory().store8(gungnir::hle::kBootArgumentAddress +
                    static_cast<std::uint32_t>(i),
                static_cast<std::uint8_t>(config.boot_path[i]));
        impl_->runtime->memory().store8(gungnir::hle::kBootArgumentAddress +
                static_cast<std::uint32_t>(config.boot_path.size()), 0u);

        if (config.stop_at_flip) {
            Impl *impl = impl_.get();
            gungnir::hle::set_display_flip_observer([impl] {
                const auto &display = gungnir::hle::display_state();
                impl->runtime->stop("first sceDisplaySetFrameBuf " + psprecomp::hex32(display.framebuffer));
            });
        }

        AllegrexContext &ctx = impl_->runtime->cpu();
        ctx.set_gpr(4, static_cast<std::uint32_t>(config.boot_path.size() + 1u));
        ctx.set_gpr(5, gungnir::hle::kBootArgumentAddress);
        gungnir::hle::kernel().start_loader_thread(ctx, impl_->entry, 0u);
        impl_->started = true;
        return true;
    } catch (const std::exception &failure) {
        error = "session_start_failed: " + std::string(failure.what());
        impl_->result.error = error;
        impl_->runtime.reset();
        impl_->hle.reset();
        impl_->started = false;
        return false;
    }
}

void GameSession::run() {
    if (!impl_->started || impl_->runtime == nullptr) {
        impl_->result.ok = false;
        impl_->result.error = "session_not_started";
        return;
    }
    impl_->started_at = std::chrono::steady_clock::now();
    g_active = impl_.get();
    std::size_t chain_capacity = 0u;
    if (const char *text = std::getenv("GUNGNIR_CHAIN_RING")) {
        if (*text != '\0') {
            const unsigned long value = std::strtoul(text, nullptr, 10);
            chain_capacity = value != 0ul ? static_cast<std::size_t>(value) : std::size_t{64};
        }
    }
    gungnir::session::configure_chain_ring(chain_capacity);
    psprecomp::set_runtime_pre_dispatch_hook(&count_dispatch);
    if (chain_capacity != 0u) {
        psprecomp::set_runtime_pre_chained_call_hook(&chain_pre);
        psprecomp::set_runtime_post_chained_call_hook(&chain_post);
    }
    psprecomp::set_runtime_heartbeat_hook(&heartbeat, 10'000u);
    if (impl_->config.stall_seconds > 0.0) {
        Impl *impl = impl_.get();
        impl->watchdog_stop.store(false, std::memory_order_relaxed);
        impl->watchdog = std::thread([impl] {
            gungnir::session::StallWatchdog watchdog(impl->config.stall_seconds);
            const auto started = std::chrono::steady_clock::now();
            while (!impl->watchdog_stop.load(std::memory_order_relaxed)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                const double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
                // Frames, not dispatches: a guest that spins in a wait loop keeps
                // dispatching while nothing reaches the screen, and that is the
                // stall a player sees.
                const std::uint64_t flips = gungnir::hle::display_state().flips;
                if (!watchdog.observe(flips, now)) continue;
                const char *inflight = gungnir::hle::inflight_import();
                const char *last = gungnir::hle::last_completed_import();
                std::string line = "guest_stall seconds=" +
                    std::to_string(static_cast<long long>(watchdog.stalled_seconds())) +
                    " flips=" + std::to_string(flips) +
                    " inflight=" + (inflight != nullptr ? inflight : "none") +
                    " last_import=" + (last != nullptr ? last : "none");
                impl->emit_diagnostic(line);
                impl->stop_requested.store(true, std::memory_order_relaxed);
            }
        });
    }
    try {
        impl_->runtime->run(impl_->entry, impl_->config.max_dispatches);
        impl_->result.ok = true;
    } catch (const std::exception &failure) {
        impl_->result.ok = false;
        impl_->result.error = "session_run_failed: " + std::string(failure.what());
        gungnir::session::dump_chain_ring(std::cerr);
        gungnir::hle::dump_hle_ring(std::cerr);
    }
    impl_->watchdog_stop.store(true, std::memory_order_relaxed);
    if (impl_->watchdog.joinable()) impl_->watchdog.join();
    clear_hooks();
    g_active = nullptr;
    impl_->fill_result();
    impl_->result.ok = impl_->result.ok && impl_->result.error.empty();
}

void GameSession::request_stop() {
    impl_->stop_requested.store(true, std::memory_order_relaxed);
}

void GameSession::set_diagnostic_sink(std::function<void(std::string_view)> sink) {
    std::lock_guard lock(impl_->diagnostic_mutex);
    impl_->diagnostic_sink = std::move(sink);
}

void GameSession::shutdown() {
    if (!impl_) return;
    impl_->watchdog_stop.store(true, std::memory_order_relaxed);
    if (impl_->watchdog.joinable()) impl_->watchdog.join();
    impl_->stop_requested.store(true, std::memory_order_relaxed);
    if (g_active == impl_.get()) g_active = nullptr;
    clear_hooks();
    gungnir::hle::set_display_flip_observer({});
    if (impl_->sink != nullptr && gungnir::audio::audio_sink() == impl_->sink)
        gungnir::audio::set_audio_sink(nullptr);
    impl_->started = false;
}

const Result &GameSession::result() const {
    return impl_->result;
}

bool GameSession::started() const {
    return impl_->started;
}

psprecomp::Runtime &GameSession::runtime() {
    return *impl_->runtime;
}

} // namespace gungnir::session

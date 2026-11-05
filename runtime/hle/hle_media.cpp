#include "hle/hle_media.hpp"

#include "hle/cadence.hpp"

#include "ge/display_list.hpp"
#include "ge/state_machine.hpp"
#include "input/input_state.hpp"
#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kEdramBase = 0x04000000u;
constexpr std::uint32_t kEdramSize = 0x00200000u;
constexpr float kFramePerSecond = 59.94f;

struct GeCallback {
    std::uint32_t signal_function{};
    std::uint32_t signal_argument{};
    std::uint32_t finish_function{};
    std::uint32_t finish_argument{};
};

struct GeList {
    std::uint32_t pc{};
    std::uint32_t stall{};
    std::int32_t callback{-1};
    bool done{};
    bool failed{};
    bool callbacks_delivered{};
    std::uint64_t commands{};
    std::uint64_t draws{};
    std::uint32_t signal{};
    std::uint32_t finish{};
    bool has_signal{};
    bool has_finish{};
    std::unique_ptr<gungnir::ge::DisplayList> runner;
};

struct MediaState {
    DisplayState display;
    std::uint32_t ctrl_cycle{};
    std::uint32_t ctrl_mode{};
    std::uint32_t ctrl_reads{};
    std::uint32_t ctrl_last_buttons{};
    std::uint32_t ctrl_latch_make{};
    std::uint32_t ctrl_latch_break{};
    std::uint32_t ctrl_latch_press{};
    std::uint32_t ctrl_latch_release{};
    std::uint32_t ctrl_latch_samples{};
    std::map<std::int32_t, GeCallback> ge_callbacks;
    std::int32_t next_ge_callback{};
    std::map<std::uint32_t, GeList> ge_lists;
    std::uint32_t next_ge_list{1u};
    gungnir::ge::StateMachine ge;
    std::vector<gungnir::ge::Draw> pending_draws;
    std::function<void(const std::vector<gungnir::ge::Draw> &)> frame_sink;
    std::set<std::string> logged_ge_gaps;
    std::uint64_t ge_frames{};
    std::uint64_t ge_gaps{};
    std::function<void()> flip_observer;
};

MediaState &media() {
    static MediaState state;
    return state;
}

bool trace_media() {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_MEDIA") != nullptr;
    return enabled;
}

// c64gg6n7 camera-path trace. The guest uploads its projection matrix as GE
// display-list commands 0x3E (matrix index) and 0x3F (matrix upload). Log each
// one with the enqueued list PC, the command's own PC and the raw word, so a
// bounded run shows which list carries the matrix and what f24 values it uses.
// Opt-in and bounded: an absent env var changes nothing, and the default line
// cap prevents a long run from producing an unbounded log.
bool trace_camera() {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_CAMERA") != nullptr;
    return enabled;
}

std::uint64_t camera_trace_limit() {
    static const std::uint64_t limit = [] {
        const char *value = std::getenv("GUNGNIR_TRACE_CAMERA_LIMIT");
        if (value == nullptr) return std::uint64_t{4096};
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return (end != value && parsed > 0) ? static_cast<std::uint64_t>(parsed)
                                            : std::uint64_t{4096};
    }();
    return limit;
}

// Vertex-source trace (per-draw identity hunt). GE transform matrices are
// constant in this title, so a stable object identity must come from the CPU-
// side vertex buffers the guest writes. Log each resolved vertex/index address
// plus the raw argument, opt-in and bounded, so a bounded run shows whether the
// same object keeps the same source address across frames.
bool trace_vertex_source() {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_VERTEX_SOURCE") != nullptr;
    return enabled;
}

std::uint64_t vertex_source_limit() {
    static const std::uint64_t limit = [] {
        const char *value = std::getenv("GUNGNIR_TRACE_VERTEX_SOURCE_LIMIT");
        if (value == nullptr) return std::uint64_t{4096};
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(value, &end, 10);
        return (end != value && parsed > 0) ? static_cast<std::uint64_t>(parsed)
                                            : std::uint64_t{4096};
    }();
    return limit;
}

void trace_vertex_source_command(const GeList &list, const gungnir::ge::Command &command) {
    static std::uint64_t lines = 0;
    if (!trace_vertex_source() || lines >= vertex_source_limit()) return;
    const bool address = command.kind == gungnir::ge::CommandKind::VertexAddress ||
                         command.kind == gungnir::ge::CommandKind::IndexAddress;
    const bool primitive = command.kind == gungnir::ge::CommandKind::Primitive;
    if (!address && !primitive) return;
    ++lines;
    std::cerr << "[vertex_source] list=" << psprecomp::hex32(list.pc)
              << " command_pc=" << psprecomp::hex32(command.pc)
              << " opcode=" << psprecomp::hex32(command.opcode)
              << " argument=" << psprecomp::hex32(command.argument)
              << " address=" << psprecomp::hex32(command.address)
              << " count=" << command.count << "\n";
}

void trace_camera_command(const GeList &list, const gungnir::ge::Command &command) {
    static std::uint64_t lines = 0;
    if (!trace_camera() || lines >= camera_trace_limit()) return;
    // Projection (0x3E/0x3F), world (0x3A/0x3B), view (0x3C/0x3D) and bone
    // (0x2A/0x2B) matrix commands. The per-draw transform stream is what a
    // matrix-based interpolator needs; the camera trace now records all of it.
    const bool projection = command.opcode == 0x3Eu || command.opcode == 0x3Fu;
    const bool transform = command.opcode == 0x3Au || command.opcode == 0x3Bu ||
                           command.opcode == 0x3Cu || command.opcode == 0x3Du ||
                           command.opcode == 0x2Au || command.opcode == 0x2Bu;
    if (!projection && !transform) return;
    ++lines;
    std::cerr << (transform ? "[camera_transform] " : "[camera_projection] ")
              << "list=" << psprecomp::hex32(list.pc)
              << " command_pc=" << psprecomp::hex32(command.pc)
              << " opcode=" << psprecomp::hex32(command.opcode)
              << " word=" << psprecomp::hex32(command.word) << "\n";
}

void deliver_ge_callbacks(GeList &list) {
    if (list.callbacks_delivered) return;
    list.callbacks_delivered = true;
    const auto callback = media().ge_callbacks.find(list.callback);
    if (callback == media().ge_callbacks.end()) return;
    if (list.has_signal && callback->second.signal_function != 0u) {
        InterruptCall call{};
        call.function = callback->second.signal_function;
        call.arguments = {0u, callback->second.signal_argument, list.pc, 0u};
        kernel().queue_interrupt(std::move(call));
    }
    if (list.has_finish && callback->second.finish_function != 0u) {
        InterruptCall call{};
        call.function = callback->second.finish_function;
        call.arguments = {0u, callback->second.finish_argument, list.pc, 0u};
        kernel().queue_interrupt(std::move(call));
    }
}

void make_ge_runner(Runtime &rt, GeList &list) {
    gungnir::ge::DisplayList::Config config;
    config.max_commands = 1'000'000u;
    config.on_command = [&list, &rt](const gungnir::ge::Command &command) {
        ++list.commands;
        trace_camera_command(list, command);
        trace_vertex_source_command(list, command);
        if (command.kind == gungnir::ge::CommandKind::Primitive) ++list.draws;
        if (command.kind == gungnir::ge::CommandKind::Signal) {
            list.signal = command.argument;
            list.has_signal = true;
        }
        if (command.kind == gungnir::ge::CommandKind::Finish) {
            list.finish = command.argument;
            list.has_finish = true;
        }
        media().ge.command(command, [&rt](std::uint32_t address, std::uint8_t *out, std::size_t size) {
            if (!rt.memory().contains(address, size)) return false;
            try {
                rt.memory().copy_out(address, std::span<std::uint8_t>(out, size));
                return true;
            } catch (const std::exception &) {
                return false;
            }
        });
    };
    gungnir::ge::WordReader reader = [&rt](std::uint32_t address) -> std::optional<std::uint32_t> {
        return rt.memory().load32(address);
    };
    list.runner = std::make_unique<gungnir::ge::DisplayList>(reader, config);
    list.runner->reset(list.pc, list.stall);
}

void run_ge_list(Runtime &rt, std::uint32_t id) {
    auto found = media().ge_lists.find(id);
    if (found == media().ge_lists.end()) return;
    GeList &list = found->second;
    if (list.done) return;
    if (!list.runner) make_ge_runner(rt, list);
    // Refreshed per kick so the stored source never outlives the session's
    // memory; the epoch bump marks a point where guest code may have run.
    media().ge.set_fast_source([&rt](std::uint32_t address, std::size_t size) -> std::span<const std::uint8_t> {
        const std::uint8_t *bytes = rt.memory().raw_pointer(address, size);
        if (bytes == nullptr) return {};
        return {bytes, size};
    });
    media().ge.begin_list();
    const gungnir::ge::RunResult result = list.runner->run();
    if (result.status == gungnir::ge::RunStatus::Stalled) return;
    if (result.status == gungnir::ge::RunStatus::Completed) {
        list.done = true;
        deliver_ge_callbacks(list);
        return;
    }
    std::cerr << "[ge] list " << id << " failed: " << gungnir::ge::run_status_name(result.status)
              << " pc=" << psprecomp::hex32(result.pc) << " word=" << psprecomp::hex32(result.word) << "\n";
    list.failed = true;
    list.done = true;
}

void write_pad_state(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t count,
    std::uint64_t now_us) {
    const gungnir::input::PadState pad = gungnir::input::state().pad();
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t entry = address + i * 16u;
        memory.store32(entry, static_cast<std::uint32_t>(now_us));
        memory.store32(entry + 4u, pad.buttons);
        // The guest subtracts 128 from Lx/Ly, so 0x80 is neutral in analog and
        // digital sampling modes alike.
        memory.store8(entry + 8u, pad.lx);
        memory.store8(entry + 9u, pad.ly);
        memory.store8(entry + 10u, pad.rx);
        memory.store8(entry + 11u, pad.ry);
        for (std::uint32_t j = 12u; j < 16u; ++j) memory.store8(entry + j, 0u);
    }
}

// sceCtrl keeps a make/break/press/release latch. The PSP samples it every
// vblank; here a buffer read is the sample point (the guest's frame loop), so
// the latch reflects input at the same moments the guest sees the pad.
void sample_ctrl_latch() {
    MediaState &state = media();
    const std::uint32_t buttons = gungnir::input::state().buttons();
    const std::uint32_t changed = buttons ^ state.ctrl_last_buttons;
    state.ctrl_latch_make |= buttons & changed;
    state.ctrl_latch_break |= state.ctrl_last_buttons & changed;
    state.ctrl_latch_press |= buttons;
    state.ctrl_latch_release |= ~buttons;
    state.ctrl_last_buttons = buttons;
    ++state.ctrl_latch_samples;
}

void write_ctrl_latch(psprecomp::GuestMemory &memory, std::uint32_t address) {
    const MediaState &state = media();
    memory.store32(address, state.ctrl_latch_make);
    memory.store32(address + 4u, state.ctrl_latch_break);
    memory.store32(address + 8u, state.ctrl_latch_press);
    memory.store32(address + 12u, state.ctrl_latch_release);
}

std::uint32_t take_ctrl_latch(psprecomp::GuestMemory &memory, std::uint32_t address) {
    const std::uint32_t previous = media().ctrl_latch_samples;
    write_ctrl_latch(memory, address);
    MediaState &state = media();
    state.ctrl_latch_make = 0u;
    state.ctrl_latch_break = 0u;
    state.ctrl_latch_press = 0u;
    state.ctrl_latch_release = 0u;
    state.ctrl_latch_samples = 0u;
    return previous;
}

void register_display(HleRegistrar &hle) {
    hle.add("sceDisplay", "sceDisplaySetMode", [](Runtime &, AllegrexContext &ctx) {
        DisplayState &display = display_state();
        display.mode = arg(ctx, 0);
        display.width = arg(ctx, 1);
        display.height = arg(ctx, 2);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceDisplay", "sceDisplaySetFrameBuf", [](Runtime &, AllegrexContext &ctx) {
        DisplayState &display = display_state();
        display.framebuffer = arg(ctx, 0);
        display.buffer_width = arg(ctx, 1);
        display.pixel_format = arg(ctx, 2);
        ++display.flips;
        if (trace_media() || display.flips == 1u)
            std::cerr << "[display] set frame buffer " << psprecomp::hex32(display.framebuffer)
                      << " width=" << display.buffer_width << " format=" << display.pixel_format << "\n";
        // A zero-width buffer is the guest turning the display off during
        // boot; only a real buffer is a presentable flip.
        if (display.buffer_width != 0u) {
            ++media().ge_frames;
            std::vector<gungnir::ge::Draw> draws;
            draws.swap(media().pending_draws);
            media().ge.end_frame();
            note_flip(draws, kernel().vblank_count());
            if (media().frame_sink) media().frame_sink(draws);
            if (media().flip_observer) media().flip_observer();
        }
        kernel().finish(ctx, 0u);
    });
    hle.add("sceDisplay", "sceDisplayGetVcount",
        [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, static_cast<std::uint32_t>(kernel().vblank_count())); });
    hle.add("sceDisplay", "sceDisplayGetFramePerSec", [](Runtime &, AllegrexContext &ctx) {
        ctx.fpr[0] = kFramePerSecond;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceDisplay", "sceDisplayIsForeground", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 1u); });
    hle.add("sceDisplay", "sceDisplayIsVblank", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });

    const auto wait_vblank = [](Runtime &, AllegrexContext &ctx) {
        WaitState wait{};
        wait.type = WaitType::VBlank;
        wait.value = 1u;
        note_vblank_wait(1u, kernel().vblank_count());
        kernel().block(ctx, wait, 0u);
    };
    hle.add("sceDisplay", "sceDisplayWaitVblankStart", wait_vblank);
    hle.add("sceDisplay", "sceDisplayWaitVblankStartCB", [wait_vblank](Runtime &rt, AllegrexContext &ctx) {
        (void)kernel().deliver_callbacks();
        wait_vblank(rt, ctx);
    });
    hle.add("sceDisplay", "sceDisplayWaitVblankStartMultiCB", [](Runtime &, AllegrexContext &ctx) {
        WaitState wait{};
        wait.type = WaitType::VBlank;
        wait.value = std::max(1u, arg(ctx, 0));
        note_vblank_wait(wait.value, kernel().vblank_count());
        kernel().block(ctx, wait, 0u);
    });
    hle.add("sceDisplay", "sceDisplayWaitVblank", wait_vblank);
}

void register_ctrl(HleRegistrar &hle) {
    hle.add("sceCtrl", "sceCtrlSetSamplingCycle", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_cycle;
        media().ctrl_cycle = arg(ctx, 0);
        kernel().finish(ctx, previous);
    });
    hle.add("sceCtrl", "sceCtrlSetSamplingMode", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t previous = media().ctrl_mode;
        media().ctrl_mode = arg(ctx, 0);
        kernel().finish(ctx, previous);
    });
    hle.add("sceCtrl", "sceCtrlSetIdleCancelThreshold", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, 0u);
    });
    hle.add("sceCtrl", "sceCtrlReadBufferPositive", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 0);
        const std::uint32_t count = std::clamp<std::uint32_t>(arg(ctx, 1), 1u, 64u);
        sample_ctrl_latch();
        write_pad_state(rt.memory(), address, count, kernel().now_us());
        ++media().ctrl_reads;
        WaitState wait{};
        wait.type = WaitType::VBlank;
        wait.value = 1u;
        kernel().block(ctx, wait, count);
    });
    hle.add("sceCtrl", "sceCtrlPeekBufferPositive", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 0);
        const std::uint32_t count = std::clamp<std::uint32_t>(arg(ctx, 1), 1u, 64u);
        sample_ctrl_latch();
        write_pad_state(rt.memory(), address, count, kernel().now_us());
        kernel().finish(ctx, count);
    });
    hle.add("sceCtrl", "sceCtrlReadLatch", [](Runtime &rt, AllegrexContext &ctx) {
        kernel().finish(ctx, take_ctrl_latch(rt.memory(), arg(ctx, 0)));
    });
    hle.add("sceCtrl", "sceCtrlPeekLatch", [](Runtime &rt, AllegrexContext &ctx) {
        sample_ctrl_latch();
        write_ctrl_latch(rt.memory(), arg(ctx, 0));
        kernel().finish(ctx, media().ctrl_latch_samples);
    });
}

void register_ge(HleRegistrar &hle) {
    hle.add("sceGe_user", "sceGeEdramGetAddr", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramBase); });
    hle.add("sceGe_user", "sceGeEdramGetSize", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, kEdramSize); });
    hle.add("sceGe_user", "sceGeEdramSetAddrTranslation",
        [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeSetCallback", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t data = arg(ctx, 0);
        auto &memory = rt.memory();
        const std::int32_t id = media().next_ge_callback++;
        media().ge_callbacks[id] = GeCallback{
            memory.load32(data), memory.load32(data + 4u), memory.load32(data + 8u), memory.load32(data + 12u)};
        kernel().finish(ctx, static_cast<std::uint32_t>(id));
    });
    hle.add("sceGe_user", "sceGeUnsetCallback", [](Runtime &, AllegrexContext &ctx) {
        media().ge_callbacks.erase(static_cast<std::int32_t>(arg(ctx, 0)));
        kernel().finish(ctx, 0u);
    });
    const auto enqueue = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = media().next_ge_list++;
        if (trace_media())
            std::cerr << "[ge] enqueue list " << id << " pc=" << psprecomp::hex32(arg(ctx, 0))
                      << " stall=" << psprecomp::hex32(arg(ctx, 1)) << " callback=" << arg(ctx, 2) << "\n";
        GeList list{};
        list.pc = arg(ctx, 0) & 0x0FFFFFFFu;
        list.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        list.callback = static_cast<std::int32_t>(arg(ctx, 2));
        auto inserted = media().ge_lists.emplace(id, std::move(list));
        make_ge_runner(rt, inserted.first->second);
        run_ge_list(rt, id);
        kernel().finish(ctx, id);
    };
    hle.add("sceGe_user", "sceGeListEnQueue", enqueue);
    hle.add("sceGe_user", "sceGeListEnQueueHead", enqueue);
    hle.add("sceGe_user", "sceGeListUpdateStallAddr", [](Runtime &rt, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        if (found == media().ge_lists.end()) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        found->second.stall = arg(ctx, 1) & 0x0FFFFFFFu;
        if (found->second.runner) found->second.runner->set_stall(found->second.stall);
        run_ge_list(rt, arg(ctx, 0));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeListSync", [](Runtime &, AllegrexContext &ctx) {
        auto found = media().ge_lists.find(arg(ctx, 0));
        // 0 = completed, 2 = still drawing.
        const std::uint32_t state = found == media().ge_lists.end() || found->second.done ? 0u : 2u;
        kernel().finish(ctx, state);
    });
    hle.add("sceGe_user", "sceGeDrawSync", [](Runtime &, AllegrexContext &ctx) {
        std::erase_if(media().ge_lists, [](const auto &item) { return item.second.done; });
        kernel().finish(ctx, 0u);
    });
    hle.add("sceGe_user", "sceGeBreak", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceGe_user", "sceGeContinue", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
}

} // namespace

namespace {
// Host presentation state survives a media reset by design: it is not part of
// the guest or of a snapshot.
float g_widescreen_scale = 1.0f;
} // namespace

DisplayState &display_state() {
    return media().display;
}

void reset_media_state() {
    media() = MediaState{};
    reset_cadence();
    MediaState &state = media();
    state.ge.set_widescreen(g_widescreen_scale);
    state.ge.set_draw_sink([&state](gungnir::ge::Draw draw) {
        state.pending_draws.push_back(std::move(draw));
    });
    state.ge.set_gap_sink([&state](const std::string &gap) {
        ++state.ge_gaps;
        if (state.logged_ge_gaps.insert(gap).second) {
            std::cerr << "[ge] gap: " << gap << "\n";
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "gungnir", "ge_gap=%s", gap.c_str());
#endif
        }
    });
}

void set_display_flip_observer(std::function<void()> observer) {
    media().flip_observer = std::move(observer);
}

void set_ge_frame_sink(std::function<void(const std::vector<gungnir::ge::Draw> &)> sink) {
    media().frame_sink = std::move(sink);
}

void set_widescreen_scale(float scale) {
    g_widescreen_scale = scale > 1.0f ? scale : 1.0f;
    media().ge.set_widescreen(g_widescreen_scale);
}

float widescreen_scale() {
    return g_widescreen_scale;
}

std::uint64_t ge_frame_count() {
    return media().ge_frames;
}

std::size_t ge_pending_draw_count() {
    return media().pending_draws.size();
}

std::uint64_t ge_gap_count() {
    return media().ge_gaps;
}

void register_media(HleRegistrar &hle) {
    register_display(hle);
    register_ctrl(hle);
    register_ge(hle);
}

namespace {

void dput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void dput_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
}

struct DReader {
    const std::uint8_t *p{};
    std::size_t left{};
    bool ok{true};

    [[nodiscard]] std::uint32_t u32() {
        if (left < 4u) {
            ok = false;
            return 0u;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
        p += 4;
        left -= 4;
        return v;
    }

    [[nodiscard]] std::uint64_t u64() {
        if (left < 8u) {
            ok = false;
            return 0u;
        }
        std::uint64_t v = 0u;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        left -= 8;
        return v;
    }

    [[nodiscard]] std::uint8_t u8() {
        if (left < 1u) {
            ok = false;
            return 0u;
        }
        const std::uint8_t v = *p;
        ++p;
        --left;
        return v;
    }
};

void dput_runner(std::vector<std::uint8_t> &out, const GeList &list) {
    const bool has_runner = list.runner != nullptr;
    out.push_back(has_runner ? 1u : 0u);
    if (!has_runner) return;
    dput_u32(out, list.runner->pc());
    dput_u32(out, list.runner->stall());
    dput_u32(out, list.runner->base());
    dput_u32(out, list.runner->offset());
    dput_u64(out, list.runner->executed());
    out.push_back(list.runner->completed() ? 1u : 0u);
    const auto calls = list.runner->call_stack();
    dput_u32(out, static_cast<std::uint32_t>(calls.size()));
    for (const auto &[return_pc, offset] : calls) {
        dput_u32(out, return_pc);
        dput_u32(out, offset);
    }
}

} // namespace

bool export_snapshot_media(std::vector<std::uint8_t> &bytes_out, std::string &error) {
    const MediaState &state = media();
    // In-flight draws are derived: the game re-submits display lists every
    // frame, so a save does not fail when a frame is mid-flight. They are
    // absent from the snapshot and dropped on load (one partial frame at
    // most); the caller logs the count for evidence.
    bytes_out.clear();
    dput_u32(bytes_out, state.display.mode);
    dput_u32(bytes_out, state.display.width);
    dput_u32(bytes_out, state.display.height);
    dput_u32(bytes_out, state.display.framebuffer);
    dput_u32(bytes_out, state.display.buffer_width);
    dput_u32(bytes_out, state.display.pixel_format);
    dput_u64(bytes_out, state.display.flips);
    dput_u32(bytes_out, state.ctrl_cycle);
    dput_u32(bytes_out, state.ctrl_mode);
    dput_u32(bytes_out, state.ctrl_reads);
    dput_u32(bytes_out, state.ctrl_last_buttons);
    dput_u32(bytes_out, state.ctrl_latch_make);
    dput_u32(bytes_out, state.ctrl_latch_break);
    dput_u32(bytes_out, state.ctrl_latch_press);
    dput_u32(bytes_out, state.ctrl_latch_release);
    dput_u64(bytes_out, state.ctrl_latch_samples);
    dput_u32(bytes_out, static_cast<std::uint32_t>(state.ge_callbacks.size()));
    for (const auto &[id, callback] : state.ge_callbacks) {
        dput_u32(bytes_out, static_cast<std::uint32_t>(id));
        dput_u32(bytes_out, callback.signal_function);
        dput_u32(bytes_out, callback.signal_argument);
        dput_u32(bytes_out, callback.finish_function);
        dput_u32(bytes_out, callback.finish_argument);
    }
    dput_u32(bytes_out, static_cast<std::uint32_t>(state.next_ge_callback));
    dput_u32(bytes_out, static_cast<std::uint32_t>(state.ge_lists.size()));
    for (const auto &[id, list] : state.ge_lists) {
        dput_u32(bytes_out, id);
        dput_u32(bytes_out, list.pc);
        dput_u32(bytes_out, list.stall);
        dput_u32(bytes_out, static_cast<std::uint32_t>(list.callback));
        bytes_out.push_back(list.done ? 1u : 0u);
        bytes_out.push_back(list.failed ? 1u : 0u);
        bytes_out.push_back(list.callbacks_delivered ? 1u : 0u);
        dput_u64(bytes_out, list.commands);
        dput_u64(bytes_out, list.draws);
        dput_u32(bytes_out, list.signal);
        dput_u32(bytes_out, list.finish);
        bytes_out.push_back(list.has_signal ? 1u : 0u);
        bytes_out.push_back(list.has_finish ? 1u : 0u);
        dput_runner(bytes_out, list);
    }
    dput_u32(bytes_out, state.next_ge_list);
    std::vector<std::uint8_t> machine;
    state.ge.export_snapshot(machine);
    dput_u32(bytes_out, static_cast<std::uint32_t>(machine.size()));
    bytes_out.insert(bytes_out.end(), machine.begin(), machine.end());
    dput_u64(bytes_out, state.ge_frames);
    dput_u64(bytes_out, state.ge_gaps);
    error.clear();
    return true;
}

bool import_snapshot_media(std::span<const std::uint8_t> bytes, Runtime &runtime, std::string &error) {
    DReader in{bytes.data(), bytes.size()};
    DisplayState display{};
    display.mode = in.u32();
    display.width = in.u32();
    display.height = in.u32();
    display.framebuffer = in.u32();
    display.buffer_width = in.u32();
    display.pixel_format = in.u32();
    display.flips = in.u64();
    std::uint32_t ctrl_cycle = in.u32();
    std::uint32_t ctrl_mode = in.u32();
    std::uint32_t ctrl_reads = in.u32();
    std::uint32_t ctrl_last_buttons = in.u32();
    std::uint32_t ctrl_latch_make = in.u32();
    std::uint32_t ctrl_latch_break = in.u32();
    std::uint32_t ctrl_latch_press = in.u32();
    std::uint32_t ctrl_latch_release = in.u32();
    std::uint64_t ctrl_latch_samples = in.u64();
    if (!in.ok) {
        error = "snapshot_truncated: media display truncated";
        return false;
    }
    const std::uint32_t callback_count = in.u32();
    if (!in.ok || callback_count > 256u) {
        error = "snapshot_truncated: ge callback count malformed";
        return false;
    }
    std::map<std::int32_t, GeCallback> ge_callbacks;
    for (std::uint32_t i = 0; i < callback_count; ++i) {
        const std::int32_t id = static_cast<std::int32_t>(in.u32());
        GeCallback callback;
        callback.signal_function = in.u32();
        callback.signal_argument = in.u32();
        callback.finish_function = in.u32();
        callback.finish_argument = in.u32();
        if (!in.ok) {
            error = "snapshot_truncated: ge callback truncated";
            return false;
        }
        if (!ge_callbacks.emplace(id, callback).second) {
            error = "snapshot_duplicate_uid: duplicate ge callback";
            return false;
        }
    }
    const std::int32_t next_ge_callback = static_cast<std::int32_t>(in.u32());
    const std::uint32_t list_count = in.u32();
    if (!in.ok || list_count > 65536u) {
        error = "snapshot_truncated: ge list count malformed";
        return false;
    }
    struct RunnerState {
        std::uint32_t pc{};
        std::uint32_t stall{};
        std::uint32_t base{};
        std::uint32_t offset{};
        std::uint64_t executed{};
        bool done{};
        std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
    };
    // Runner closures capture their list by reference, so runners are rebuilt
    // on the committed map elements after the move, never on temporaries.
    std::map<std::uint32_t, RunnerState> runners;
    std::map<std::uint32_t, GeList> ge_lists;
    for (std::uint32_t i = 0; i < list_count; ++i) {
        const std::uint32_t id = in.u32();
        GeList list;
        list.pc = in.u32();
        list.stall = in.u32();
        list.callback = static_cast<std::int32_t>(in.u32());
        const std::uint8_t done = in.u8();
        const std::uint8_t failed = in.u8();
        const std::uint8_t delivered = in.u8();
        list.commands = in.u64();
        list.draws = in.u64();
        list.signal = in.u32();
        list.finish = in.u32();
        const std::uint8_t has_signal = in.u8();
        const std::uint8_t has_finish = in.u8();
        const std::uint8_t has_runner = in.u8();
        if (!in.ok || done > 1u || failed > 1u || delivered > 1u || has_signal > 1u || has_finish > 1u ||
            has_runner > 1u) {
            error = "snapshot_bad_length: ge list malformed";
            return false;
        }
        list.done = done != 0u;
        list.failed = failed != 0u;
        list.callbacks_delivered = delivered != 0u;
        list.has_signal = has_signal != 0u;
        list.has_finish = has_finish != 0u;
        if (has_runner != 0u) {
            const std::uint32_t runner_pc = in.u32();
            const std::uint32_t runner_stall = in.u32();
            const std::uint32_t runner_base = in.u32();
            const std::uint32_t runner_offset = in.u32();
            const std::uint64_t runner_executed = in.u64();
            const std::uint8_t runner_done = in.u8();
            const std::uint32_t call_count = in.u32();
            if (!in.ok || runner_done > 1u || call_count > 64u) {
                error = "snapshot_bad_length: ge runner malformed";
                return false;
            }
            std::vector<std::pair<std::uint32_t, std::uint32_t>> calls;
            calls.reserve(call_count);
            for (std::uint32_t c = 0; c < call_count; ++c) {
                const std::uint32_t return_pc = in.u32();
                const std::uint32_t call_offset = in.u32();
                if (!in.ok) {
                    error = "snapshot_truncated: ge call stack truncated";
                    return false;
                }
                calls.emplace_back(return_pc, call_offset);
            }
            RunnerState pending{runner_pc, runner_stall, runner_base, runner_offset, runner_executed,
                runner_done != 0u, std::move(calls)};
            if (!runners.emplace(id, std::move(pending)).second) {
                error = "snapshot_duplicate_uid: duplicate ge list";
                return false;
            }
        }
        if (!ge_lists.emplace(id, std::move(list)).second) {
            error = "snapshot_duplicate_uid: duplicate ge list";
            return false;
        }
    }
    const std::uint32_t next_ge_list = in.u32();
    const std::uint32_t machine_len = in.u32();
    if (!in.ok || machine_len == 0u || machine_len > 65536u || in.left < machine_len) {
        error = "snapshot_truncated: ge state machine truncated";
        return false;
    }
    gungnir::ge::StateMachine machine;
    if (!machine.import_snapshot(std::span<const std::uint8_t>(in.p, machine_len), error)) return false;
    in.p += machine_len;
    in.left -= machine_len;
    const std::uint64_t ge_frames = in.u64();
    const std::uint64_t ge_gaps = in.u64();
    if (!in.ok || in.left != 0u) {
        error = "snapshot_truncated: trailing bytes in media section";
        return false;
    }
    MediaState &state = media();
    state.display = display;
    state.ctrl_cycle = ctrl_cycle;
    state.ctrl_mode = ctrl_mode;
    state.ctrl_reads = ctrl_reads;
    state.ctrl_last_buttons = ctrl_last_buttons;
    state.ctrl_latch_make = ctrl_latch_make;
    state.ctrl_latch_break = ctrl_latch_break;
    state.ctrl_latch_press = ctrl_latch_press;
    state.ctrl_latch_release = ctrl_latch_release;
    state.ctrl_latch_samples = ctrl_latch_samples;
    state.ge_callbacks = std::move(ge_callbacks);
    state.next_ge_callback = next_ge_callback;
    state.ge_lists.clear();
    for (auto &[id, list] : ge_lists) state.ge_lists.emplace(id, std::move(list));
    state.next_ge_list = next_ge_list;
    for (const auto &[id, runner] : runners) {
        const auto found = state.ge_lists.find(id);
        if (found == state.ge_lists.end()) {
            error = "snapshot_incomplete: runner without list";
            return false;
        }
        make_ge_runner(runtime, found->second);
        found->second.runner->restore_execution(runner.pc, runner.stall, runner.base, runner.offset,
            runner.calls, runner.executed, runner.done);
    }
    state.pending_draws.clear();
    state.logged_ge_gaps.clear();
    state.ge_frames = ge_frames;
    state.ge_gaps = ge_gaps;
    state.ge = std::move(machine);
    state.ge.set_widescreen(g_widescreen_scale);
    state.ge.set_draw_sink([&state](gungnir::ge::Draw draw) { state.pending_draws.push_back(std::move(draw)); });
    state.ge.set_gap_sink([&state](const std::string &gap) {
        ++state.ge_gaps;
        if (state.logged_ge_gaps.insert(gap).second) {
            std::cerr << "[ge] gap: " << gap << "\n";
#if defined(__ANDROID__)
            __android_log_print(ANDROID_LOG_INFO, "gungnir", "ge_gap=%s", gap.c_str());
#endif
        }
    });
    error.clear();
    return true;
}

} // namespace gungnir::hle

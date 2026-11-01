#include "hle/hle_media.hpp"

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
            if (media().frame_sink) {
                std::vector<gungnir::ge::Draw> draws = std::move(media().pending_draws);
                media().pending_draws.clear();
                media().frame_sink(draws);
            }
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

DisplayState &display_state() {
    return media().display;
}

void reset_media_state() {
    media() = MediaState{};
    MediaState &state = media();
    state.ge.set_draw_sink([&state](const gungnir::ge::Draw &draw) {
        state.pending_draws.push_back(draw);
    });
    state.ge.set_gap_sink([&state](const std::string &gap) {
        ++state.ge_gaps;
        if (state.logged_ge_gaps.insert(gap).second)
            std::cerr << "[ge] gap: " << gap << "\n";
    });
}

void set_display_flip_observer(std::function<void()> observer) {
    media().flip_observer = std::move(observer);
}

void set_ge_frame_sink(std::function<void(const std::vector<gungnir::ge::Draw> &)> sink) {
    media().frame_sink = std::move(sink);
}

std::uint64_t ge_frame_count() {
    return media().ge_frames;
}

std::uint64_t ge_gap_count() {
    return media().ge_gaps;
}

void register_media(HleRegistrar &hle) {
    register_display(hle);
    register_ctrl(hle);
    register_ge(hle);
}

} // namespace gungnir::hle

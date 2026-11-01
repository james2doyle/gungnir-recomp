// T6c fixtures: display, GE submission seam, controller and RTC handlers.
//
// Usage: hle_media_tests --nids CSV
// Exit 0 on success, 1 on a failed contract, 2 on usage.

#include "hle/hle_common.hpp"
#include "hle/hle_media.hpp"
#include "hle/kernel.hpp"
#include "input/input_state.hpp"

#include "ge/display_list.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>

namespace {

using gungnir::hle::AllegrexContext;
using gungnir::hle::HleRegistrar;
using gungnir::hle::Kernel;
using gungnir::hle::Runtime;
using gungnir::hle::kernel;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::filesystem::path test_nids_path() {
    const char *path = std::getenv("GUNGNIR_TEST_NIDS_CSV");
    return path != nullptr ? std::filesystem::path(path) : std::filesystem::path();
}

class Fixture {
public:
    static constexpr std::uint32_t kText = 0x08810000u;
    static constexpr std::uint32_t kOutput = 0x08811000u;
    static constexpr std::uint32_t kReturnAddress = 0x08822000u;

    explicit Fixture(const std::filesystem::path &nids) : runtime(32u * 1024u * 1024u) {
        gungnir::hle::reset_media_state();
        gungnir::input::reset_state();
        kernel() = Kernel{};
        kernel().install(runtime, 0x08801000u, 0x08820000u);
        kernel().set_pacing_enabled(false);
        runtime.nids().load_csv(nids);
        for (const auto &symbol : runtime.nids().all())
            nids_by_name[{symbol.library, symbol.name}] = symbol.nid;
        HleRegistrar hle(runtime);
        gungnir::hle::register_system(hle);
        gungnir::hle::register_media(hle);
        kernel().start_loader_thread(ctx, 0x08820000u, 0u);
    }

    ~Fixture() {
        kernel() = Kernel{};
        psprecomp::set_runtime_starvation_hook(nullptr, 0);
        gungnir::hle::reset_media_state();
        gungnir::input::reset_state();
    }

    std::uint32_t call(const std::string &library, const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        for (unsigned i = 0; i < 8u; ++i) ctx.set_gpr(i + 4u, 0u);
        unsigned index = 0;
        for (const std::uint32_t value : args) ctx.set_gpr(4u + index++, value);
        ctx.set_gpr(31, kReturnAddress);
        runtime.invoke_import(library, nids_by_name.at({library, name}), ctx);
        return ctx.gpr[2];
    }

    std::uint32_t display(const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        return call("sceDisplay", name, args);
    }

    std::uint32_t ge(const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return call("sceGe_user", name, args);
    }

    Runtime runtime;
    Kernel &k = kernel();
    AllegrexContext &ctx = runtime.cpu();
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name;
};

void display_contracts() {
    Fixture f(test_nids_path());
    auto &display = gungnir::hle::display_state();

    check(f.display("sceDisplaySetMode", {0u, 480u, 272u}) == 0u, "set mode succeeds");
    check(display.mode == 0u && display.width == 480u && display.height == 272u,
          "mode geometry recorded");

    check(f.display("sceDisplaySetFrameBuf", {0x04088000u, 512u, 3u}) == 0u, "set frame buffer succeeds");
    check(display.framebuffer == 0x04088000u && display.buffer_width == 512u && display.pixel_format == 3u,
          "frame buffer recorded");
    check(display.flips == 1u, "one flip counted");

    check(f.display("sceDisplayGetVcount") == 0u, "vcount starts at zero");
    check(f.display("sceDisplayWaitVblankStart") == 0u, "vblank wait returns zero");
    check(f.k.vblank_count() == 1u, "vblank wait consumed one vblank");
    check(f.k.now_us() == gungnir::hle::kVBlankPeriodUs, "vblank wait advances the virtual clock");
    check(f.display("sceDisplayGetVcount") == 1u, "vcount follows vblanks");

    check(f.display("sceDisplayWaitVblankStartMultiCB", {3u}) == 0u, "multi vblank wait returns zero");
    check(f.k.vblank_count() == 4u, "multi vblank wait consumed three vblanks");
    check(f.k.now_us() == 4u * gungnir::hle::kVBlankPeriodUs, "multi wait advances three periods");

    check(f.display("sceDisplayGetFramePerSec") == 0u, "frame rate returns in f0");
    check(std::fabs(f.ctx.fpr[0] - 59.94f) < 0.01f, "frame rate is 59.94 Hz");
}

void controller_contracts() {
    Fixture f(test_nids_path());
    auto &mem = f.runtime.memory();

    check(f.call("sceCtrl", "sceCtrlSetSamplingCycle", {0u}) == 0u, "sampling cycle returns previous");
    check(f.call("sceCtrl", "sceCtrlSetSamplingCycle", {1u}) == 0u, "sampling cycle updated");
    check(f.call("sceCtrl", "sceCtrlSetSamplingMode", {0u}) == 0u, "sampling mode returns previous");
    check(f.call("sceCtrl", "sceCtrlSetIdleCancelThreshold", {0u, 0u}) == 0u, "idle threshold accepted");

    const std::uint32_t before = static_cast<std::uint32_t>(f.k.now_us());
    check(f.call("sceCtrl", "sceCtrlReadBufferPositive", {Fixture::kOutput, 2u}) == 2u,
          "pad read returns the requested sample count");
    check(f.k.vblank_count() == 1u, "pad read waits for the next sample");
    for (std::uint32_t i = 0; i < 2u; ++i) {
        const std::uint32_t entry = Fixture::kOutput + i * 16u;
        check(mem.load32(entry + 4u) == 0u, "no buttons held");
        check(mem.load8(entry + 8u) == 0x80u && mem.load8(entry + 9u) == 0x80u,
              "primary stick is centred");
        check(mem.load8(entry + 10u) == 0x80u && mem.load8(entry + 11u) == 0x80u,
              "secondary stick is centred");
    }
    check(mem.load32(Fixture::kOutput) >= before, "sample time is current virtual time");

    auto &input = gungnir::input::state();
    const gungnir::input::ButtonMask cross = gungnir::input::button_mask(gungnir::input::Button::Cross);
    const gungnir::input::ButtonMask up = gungnir::input::button_mask(gungnir::input::Button::Up);

    input.set_buttons(gungnir::input::Source::Touch, 0, 1, cross | up);
    input.set_analog(gungnir::input::Source::Touch, 0, 1, {0.5f, -0.5f});
    check(f.call("sceCtrl", "sceCtrlPeekBufferPositive", {Fixture::kOutput, 2u}) == 2u,
          "peek returns the requested samples");
    check(mem.load32(Fixture::kOutput + 4u) == (cross | up), "peek exposes held buttons");
    check(mem.load8(Fixture::kOutput + 8u) == 183u && mem.load8(Fixture::kOutput + 9u) == 73u,
          "peek exposes the merged analog through the dead zone");
    check(mem.load8(Fixture::kOutput + 10u) == 0x80u && mem.load8(Fixture::kOutput + 11u) == 0x80u,
          "unused secondary stick stays centred");

    input.release_all();
    check(f.call("sceCtrl", "sceCtrlPeekBufferPositive", {Fixture::kOutput, 1u}) == 1u,
          "peek after release returns");
    check(mem.load32(Fixture::kOutput + 4u) == 0u, "release-all clears the guest buttons");
    check(mem.load8(Fixture::kOutput + 8u) == 0x80u && mem.load8(Fixture::kOutput + 9u) == 0x80u,
          "release-all centres the guest analog");

    gungnir::hle::reset_media_state();
    const std::uint32_t latch = Fixture::kOutput + 0x40u;
    check(f.call("sceCtrl", "sceCtrlReadLatch", {latch}) == 0u, "a fresh latch has no samples");
    input.set_buttons(gungnir::input::Source::Touch, 0, 1, cross);
    check(f.call("sceCtrl", "sceCtrlPeekBufferPositive", {Fixture::kOutput, 1u}) == 1u,
          "peek samples for the latch");
    check(f.call("sceCtrl", "sceCtrlReadLatch", {latch}) == 1u, "read latch returns the sample count");
    check(mem.load32(latch) == cross, "latch records the make edge");
    check(mem.load32(latch + 4u) == 0u, "latch has no break edge");
    check(mem.load32(latch + 8u) == cross, "latch records the pressed state");
    check(f.call("sceCtrl", "sceCtrlReadLatch", {latch}) == 0u, "read latch resets the history");
    check(mem.load32(latch) == 0u && mem.load32(latch + 8u) == 0u, "reset latch is empty");

    input.release(gungnir::input::Source::Touch, 0, 1);
    check(f.call("sceCtrl", "sceCtrlReadBufferPositive", {Fixture::kOutput, 1u}) == 1u,
          "buffer read returns after a vblank");
    check(f.call("sceCtrl", "sceCtrlReadLatch", {latch}) == 1u, "buffer read samples the latch");
    check(mem.load32(latch) == 0u, "latch has no make edge after release");
    check(mem.load32(latch + 4u) == cross, "latch records the break edge");
    check(mem.load32(latch + 12u) == ~0u, "latch records release as the inverse mask");
}

int g_signal_calls = 0;
int g_finish_calls = 0;
std::uint32_t g_signal_argument = 0;
std::uint32_t g_finish_argument = 0;
std::uint32_t g_list_pc = 0;

void signal_handler(Runtime &, AllegrexContext &ctx) {
    ++g_signal_calls;
    g_signal_argument = ctx.gpr[5];
    g_list_pc = ctx.gpr[6];
    kernel().interrupt_return_stub(ctx);
}

void finish_handler(Runtime &, AllegrexContext &ctx) {
    ++g_finish_calls;
    g_finish_argument = ctx.gpr[5];
    kernel().interrupt_return_stub(ctx);
}

void ge_contracts() {
    Fixture f(test_nids_path());
    auto &mem = f.runtime.memory();
    g_signal_calls = g_finish_calls = 0;
    g_signal_argument = g_finish_argument = 0;
    g_list_pc = 0;

    check(f.ge("sceGeEdramGetAddr") == 0x04000000u, "EDRAM base");
    check(f.ge("sceGeEdramGetSize") == 0x00200000u, "EDRAM size");
    check(f.ge("sceGeEdramSetAddrTranslation", {0u}) == 0u, "EDRAM translation accepted");

    f.runtime.register_function(0x08825000u, &signal_handler, "ge-signal");
    f.runtime.register_function(0x08825004u, &finish_handler, "ge-finish");
    mem.store32(Fixture::kText, 0x08825000u);
    mem.store32(Fixture::kText + 4u, 0xAAu);
    mem.store32(Fixture::kText + 8u, 0x08825004u);
    mem.store32(Fixture::kText + 12u, 0xBBu);
    mem.store32(0x08826000u, 0x0E000000u);
    mem.store32(0x08826004u, 0x0F000000u);
    mem.store32(0x08826008u, 0x0C000000u);

    const std::uint32_t callback = f.ge("sceGeSetCallback", {Fixture::kText});
    check(static_cast<std::int32_t>(callback) >= 0, "GE callback registered");

    (void)f.ge("sceGeListEnQueue", {0x08826000u, 0x08826010u, callback, 0u});
    check(f.ctx.pc == 0x08825000u, "GE signal callback becomes the next dispatch");
    f.runtime.run(0x08825000u, 1u);
    check(g_signal_calls == 1 && g_signal_argument == 0xAAu && g_list_pc == 0x08826000u,
          "signal callback receives its argument and list address");
    check(f.ctx.pc == 0x08825004u, "GE finish callback follows the signal");
    f.runtime.run(0x08825004u, 1u);
    check(g_finish_calls == 1 && g_finish_argument == 0xBBu, "finish callback receives its argument");
    check(f.ctx.pc == Fixture::kReturnAddress, "enqueue resumes after both interrupts");
    const std::uint32_t list = f.ctx.gpr[2];
    check(static_cast<std::int32_t>(list) > 0, "enqueue returns the list id to the resumed guest");

    check(f.ge("sceGeListSync", {list, 0u}) == 0u, "completed list syncs immediately");
    check(f.ge("sceGeListUpdateStallAddr", {list, 0x08826014u}) == 0u, "stall address update accepted");

    mem.store32(0x08827000u, 0x00000000u);
    mem.store32(0x08827004u, 0x00000000u);
    mem.store32(0x08827008u, 0x0C000000u);
    (void)f.ge("sceGeListEnQueue", {0x08827000u, 0x08827008u, 0xFFFFFFFFu, 0u});
    const std::uint32_t stalled = f.ctx.gpr[2];
    check(f.ge("sceGeListSync", {stalled, 0u}) == 2u, "a list stopped at its stall address is still drawing");
    check(f.ge("sceGeListUpdateStallAddr", {stalled, 0u}) == 0u, "clearing the stall resumes the list");
    check(f.ge("sceGeListSync", {stalled, 0u}) == 0u, "the resumed list completes");

    check(f.ge("sceGeDrawSync", {0u}) == 0u, "draw sync completes lists");
    check(f.ge("sceGeBreak", {0u}) == 0u, "GE break accepted");
    check(f.ge("sceGeContinue") == 0u, "GE continue accepted");
    check(f.ge("sceGeUnsetCallback", {callback}) == 0u, "GE callback removed");
}

void draw_retention_contracts() {
    Fixture f(test_nids_path());
    auto &mem = f.runtime.memory();
    const std::uint32_t vertices = 0x08830000u;
    const std::uint32_t vertex_type = (3u << 7u) | 3u | (7u << 2u) | (1u << 23u);
    const gungnir::ge::VertexLayout layout = gungnir::ge::decode_vertex_layout(vertex_type);
    check(layout.through, "fixture vertex type is through-mode");
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t at = vertices + static_cast<std::uint32_t>(i) * layout.stride;
        for (int component = 0; component < 3; ++component) {
            std::uint32_t bits = 0;
            const float value = component == 0 ? 10.0f + static_cast<float>(i) * 20.0f : 20.0f;
            std::memcpy(&bits, &value, sizeof bits);
            mem.store32(at + layout.position_offset + static_cast<std::uint32_t>(component) * 4u, bits);
        }
        mem.store32(at + layout.color_offset, 0xFF00FF00u);
    }
    // BASE carries the address above the 24-bit command word: 0x08800000 is
    // encoded as 0x00088000, so VADDR only has to name the low half.
    mem.store32(Fixture::kText, 0x10088000u);
    mem.store32(Fixture::kText + 4u, 0x12000000u | vertex_type);
    mem.store32(Fixture::kText + 8u, 0x01000000u | (vertices & 0xFFFFu));
    mem.store32(Fixture::kText + 12u, 0x04000000u | ((3u << 16u) | 3u));
    mem.store32(Fixture::kText + 16u, 0x0C000000u);
    (void)f.ge("sceGeListEnQueue", {Fixture::kText, Fixture::kText + 20u, 0xFFFFFFFFu, 0u});
    check(gungnir::hle::ge_pending_draw_count() == 1u, "the list translates one draw");

    // No frame sink: the flip must drop the draws rather than keep them alive
    // (the Linux boot runner sets no sink, so retained draws grow per frame).
    check(f.display("sceDisplaySetFrameBuf", {0x04088000u, 512u, 3u}) == 0u, "flip accepted");
    check(gungnir::hle::ge_pending_draw_count() == 0u, "a flip without a sink drops the pending draws");

    std::size_t delivered = 0u;
    gungnir::hle::set_ge_frame_sink([&delivered](const std::vector<gungnir::ge::Draw> &draws) {
        delivered += draws.size();
    });
    (void)f.ge("sceGeListEnQueue", {Fixture::kText, Fixture::kText + 20u, 0xFFFFFFFFu, 0u});
    check(f.display("sceDisplaySetFrameBuf", {0x04088000u, 512u, 3u}) == 0u, "second flip accepted");
    check(delivered == 1u, "the sink receives the draw");
    check(gungnir::hle::ge_pending_draw_count() == 0u, "delivery clears the pending draws");
    gungnir::hle::set_ge_frame_sink(nullptr);
}

void rtc_contracts() {
    Fixture f(test_nids_path());
    check(f.call("sceRtc", "sceRtcGetCurrentTick", {Fixture::kOutput}) == 0u, "RTC tick succeeds");
    const std::uint64_t tick = (static_cast<std::uint64_t>(f.runtime.memory().load32(Fixture::kOutput + 4u)) << 32u) |
        f.runtime.memory().load32(Fixture::kOutput);
    check(tick != 0u, "RTC tick is populated");
}

} // namespace

int main(int argc, char **argv) {
    std::string nids;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--nids" && i + 1 < argc) nids = argv[++i];
        else {
            std::cerr << "usage: hle_media_tests --nids CSV\n";
            return 2;
        }
    }
    if (nids.empty()) return 2;
    setenv("GUNGNIR_TEST_NIDS_CSV", nids.c_str(), 1);
    try {
        display_contracts();
        controller_contracts();
        ge_contracts();
        draw_retention_contracts();
        rtc_contracts();
        std::cout << "hle_media_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

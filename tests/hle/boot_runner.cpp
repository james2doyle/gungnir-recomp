// Bounded Linux boot of the recompiled main module (plan T6c, TC-6.2).
//
// Loads the real decrypted ELF at its PSP load base, registers the generated
// AOT corpus and the adapted kernel/HLE boot set, and runs from the module
// entry under explicit dispatch and wall-clock bounds. The run stops at the
// first sceDisplaySetFrameBuf, which is the T6c evidence point.
//
// Exit: 0 reached the first frame buffer, 1 a missing/failed HLE import or
// guest fault stopped the run, 2 usage/input error, 3 bounded out before the
// first flip.

#include "hle/hle_common.hpp"
#include "hle/hle_media.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace {

using gungnir::hle::AllegrexContext;
using gungnir::hle::HleRegistrar;
using gungnir::hle::Runtime;
using psprecomp::Elf32Image;
using psprecomp::hex32;
using psprecomp::PspImport;
using psprecomp::PspModuleInfo;

constexpr std::uint32_t kBootArgumentAddress = 0x08100000u;

Runtime *g_runtime = nullptr;
std::chrono::steady_clock::time_point g_start{};
double g_wall_seconds = 0.0;
std::uint64_t g_outer_dispatches = 0u;

void count_dispatch(Runtime &, AllegrexContext &, std::uint32_t, std::int32_t) {
    ++g_outer_dispatches;
}

void heartbeat(std::uint64_t, std::uint32_t) {
    if (g_wall_seconds <= 0.0 || g_runtime == nullptr) return;
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    if (elapsed >= g_wall_seconds) g_runtime->stop("wall-clock limit reached");
}

[[noreturn]] void usage(const char *message) {
    std::cerr << "boot_runner: " << message << "\n"
              << "usage: boot_runner --elf FILE --nids CSV --disc FILE --ms DIR\n"
                 "                   [--load-base HEX] [--max-dispatches N]\n"
                 "                   [--wall-seconds N] [--boot-path TEXT]\n"
                 "                   [--trace-out FILE] [--no-stop-at-flip]\n";
    std::exit(2);
}

std::uint32_t parse_hex(const std::string &text) {
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
}

std::string json_escape(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) >= 0x20u) out.push_back(c);
    }
    return out;
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

} // namespace

int main(int argc, char **argv) {
    std::string elf_path;
    std::string nids_path;
    std::string disc_path;
    std::string ms_path;
    std::string boot_path = "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN";
    std::string trace_path;
    std::uint32_t load_base = psprecomp::kDefaultPspUserLoadBase;
    std::uint64_t max_dispatches = 50'000'000u;
    bool stop_at_flip = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) usage((std::string(name) + " needs a value").c_str());
            return argv[++i];
        };
        if (arg == "--elf") elf_path = next("--elf");
        else if (arg == "--nids") nids_path = next("--nids");
        else if (arg == "--disc") disc_path = next("--disc");
        else if (arg == "--ms") ms_path = next("--ms");
        else if (arg == "--load-base") load_base = parse_hex(next("--load-base"));
        else if (arg == "--max-dispatches") max_dispatches = std::stoull(next("--max-dispatches"));
        else if (arg == "--wall-seconds") g_wall_seconds = std::stod(next("--wall-seconds"));
        else if (arg == "--boot-path") boot_path = next("--boot-path");
        else if (arg == "--trace-out") trace_path = next("--trace-out");
        else if (arg == "--no-stop-at-flip") stop_at_flip = false;
        else usage(("unknown argument: " + arg).c_str());
    }
    if (elf_path.empty() || nids_path.empty()) usage("--elf and --nids are required");
    if (max_dispatches == 0u) usage("--max-dispatches must be non-zero");

    try {
        setenv("PSPRECOMP_HLE_HISTOGRAM", "1", 1);
        g_start = std::chrono::steady_clock::now();

        Runtime runtime;
        g_runtime = &runtime;
        gungnir::hle::load_nid_registry(runtime, nids_path);

        const Elf32Image elf = Elf32Image::from_file(elf_path);
        const std::uint32_t entry = elf.runtime_entry(load_base);
        (void)elf.load_and_relocate(runtime.memory(), load_base);
        const auto module = elf.find_module_info(runtime.memory(), load_base);
        if (!module)
            throw psprecomp::Error("PSP module info not found in " + elf_path);
        const std::uint32_t end = image_end(elf, load_base);

        psprecomp::register_generated_functions(runtime);
        gungnir::hle::kernel() = gungnir::hle::Kernel{};
        gungnir::hle::kernel().install(runtime, module->gp, end);
        gungnir::hle::kernel().set_pacing_enabled(false);
        gungnir::hle::reset_media_state();

        HleRegistrar hle(runtime);
        gungnir::hle::register_sysmem(hle);
        gungnir::hle::register_threadman(hle);
        gungnir::hle::register_system(hle);
        gungnir::hle::register_io(hle, disc_path, ms_path);
        gungnir::hle::register_media(hle);

        const auto imports = elf.scan_imports(runtime.memory(), *module);
        std::vector<const PspImport *> unbound;
        for (const PspImport &import : imports)
            if (!hle.bound(import.library, import.nid)) unbound.push_back(&import);

        for (std::size_t i = 0; i < boot_path.size(); ++i)
            runtime.memory().store8(kBootArgumentAddress + static_cast<std::uint32_t>(i),
                static_cast<std::uint8_t>(boot_path[i]));
        runtime.memory().store8(kBootArgumentAddress + static_cast<std::uint32_t>(boot_path.size()), 0u);

        if (stop_at_flip) {
            gungnir::hle::set_display_flip_observer([&runtime] {
                const auto &display = gungnir::hle::display_state();
                runtime.stop("first sceDisplaySetFrameBuf " + hex32(display.framebuffer));
            });
        }

        AllegrexContext &ctx = runtime.cpu();
        ctx.set_gpr(4, static_cast<std::uint32_t>(boot_path.size() + 1u));
        ctx.set_gpr(5, kBootArgumentAddress);
        gungnir::hle::kernel().start_loader_thread(ctx, entry, 0u);

        psprecomp::set_runtime_pre_dispatch_hook(&count_dispatch);
        psprecomp::set_runtime_heartbeat_hook(&heartbeat, 10'000u);
        runtime.run(entry, max_dispatches);
        psprecomp::set_runtime_heartbeat_hook(nullptr, 0u);
        psprecomp::set_runtime_pre_dispatch_hook(nullptr);

        const std::string &reason = runtime.stop_reason();
        const bool stopped_at_flip = reason.starts_with("first sceDisplaySetFrameBuf");
        const bool missing_import = reason.find("Missing HLE import") != std::string::npos;
        const auto &display = gungnir::hle::display_state();

        std::ofstream out;
        if (!trace_path.empty()) {
            out.open(trace_path);
            if (!out) usage("cannot open trace output");
        }
        std::ostream &report = trace_path.empty() ? std::cout : out;
        report << "{\n"
               << "  \"module\": \"" << json_escape(module->name) << "\",\n"
               << "  \"entry\": \"" << hex32(entry) << "\",\n"
               << "  \"gp\": \"" << hex32(module->gp) << "\",\n"
               << "  \"image_end\": \"" << hex32(end) << "\",\n"
               << "  \"functions\": " << runtime.function_count() << ",\n"
               << "  \"dispatches\": " << g_outer_dispatches << ",\n"
               << "  \"dispatch_work\": " << runtime.dispatch_work_count() << ",\n"
               << "  \"vblanks\": " << gungnir::hle::kernel().vblank_count() << ",\n"
               << "  \"virtual_us\": " << gungnir::hle::kernel().now_us() << ",\n"
               << "  \"reached_first_frame_buf\": " << (stopped_at_flip ? "true" : "false") << ",\n"
               << "  \"flips\": " << display.flips << ",\n"
               << "  \"first_frame_buf\": \"" << hex32(display.framebuffer) << "\",\n"
               << "  \"display_width\": " << display.buffer_width << ",\n"
               << "  \"display_format\": " << display.pixel_format << ",\n"
               << "  \"hle_bound\": " << hle.count() << ",\n"
               << "  \"imports_total\": " << imports.size() << ",\n"
               << "  \"imports_bound\": " << imports.size() - unbound.size() << ",\n"
               << "  \"imports_unbound\": [";
        for (std::size_t i = 0; i < unbound.size(); ++i) {
            if (i != 0u) report << ", ";
            report << "\"" << json_escape(unbound[i]->library) << ":" << hex32(unbound[i]->nid) << "\"";
        }
        report << "],\n  \"imports_executed\": [";
        const auto executed = runtime.hle_histogram();
        for (std::size_t i = 0; i < executed.size(); ++i) {
            if (i != 0u) report << ", ";
            report << "\"" << json_escape(executed[i].first) << "\"";
        }
        report << "],\n"
               << "  \"stopped\": " << (runtime.stopped() ? "true" : "false") << ",\n"
               << "  \"stop_reason\": \"" << json_escape(reason) << "\",\n"
               << "  \"stop_pc\": \"" << hex32(runtime.cpu().pc) << "\"\n"
               << "}\n";
        if (out) out.close();

        if (missing_import) return 1;
        if (!stopped_at_flip) return 3;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "boot_runner: " << error.what() << "\n";
        return 1;
    }
}

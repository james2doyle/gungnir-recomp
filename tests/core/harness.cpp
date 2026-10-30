// Synthetic Allegrex fixture harness (plan T4).
//
// Runs one fixture ELF either under the interpreter tier (no generated code
// registered; the vendored interpreter fallback executes the guest code) or
// under the AOT tier (yakumo_codegen output registered), recording a
// function-boundary trace: at every dispatch whose PC is a declared trace
// point, a SHA-256 digest of full architectural state plus declared memory
// windows. Both modes emit the same file format so the driver can require
// byte equality (TC-4.1). Loud stops are surfaced verbatim (TC-4.2).

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/interpreter.hpp"
#include "psprecomp/runtime.hpp"
#include "psprecomp/sha256.hpp"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace {

using psprecomp::AllegrexContext;
using psprecomp::GuestMemory;
using psprecomp::Runtime;
using psprecomp::hex32;

struct Range {
    std::uint32_t start;
    std::uint32_t end;
};

struct Harness {
    Runtime runtime;
    std::uint32_t load_base{psprecomp::kDefaultPspUserLoadBase};
    std::set<std::uint32_t> trace_pcs;
    std::vector<Range> digest_ranges;
    std::ofstream trace;
};

Harness *g_harness = nullptr;

std::string digest(const Runtime &rt) {
    const AllegrexContext &ctx = rt.cpu();
    std::vector<std::uint8_t> state;
    auto append = [&](const void *data, std::size_t size) {
        const auto *bytes = static_cast<const std::uint8_t *>(data);
        state.insert(state.end(), bytes, bytes + size);
    };
    append(ctx.gpr.data(), ctx.gpr.size() * sizeof(std::uint32_t));
    append(&ctx.hi, sizeof(ctx.hi));
    append(&ctx.lo, sizeof(ctx.lo));
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    append(ctx.fpr.data(), ctx.fpr.size() * sizeof(float));
    append(ctx.vfpu.data(), ctx.vfpu.size() * sizeof(float));
    append(ctx.vfpu_ctrl.data(), ctx.vfpu_ctrl.size() * sizeof(std::uint32_t));
    for (const Range &range : g_harness->digest_ranges) {
        std::vector<std::uint8_t> window(range.end - range.start);
        rt.memory().copy_out(range.start, window);
        append(window.data(), window.size());
    }
    return psprecomp::sha256_bytes(state);
}

void record(const char *what, std::uint32_t pc) {
    g_harness->trace << what << " " << hex32(pc) << " " << digest(g_harness->runtime)
                     << "\n";
}

void pre_dispatch(Runtime &rt, AllegrexContext &, std::uint32_t dispatch_pc,
                  std::int32_t) {
    if (g_harness->trace_pcs.count(dispatch_pc) != 0u) record("pre", dispatch_pc);
    (void)rt;
}

std::uint32_t runtime_address(Harness &h, std::uint32_t address) {
    (void)h;
    return address; // fixtures are linked at absolute user addresses
}

void sentinel(Runtime &rt, AllegrexContext &) {
    rt.stop("fixture-done");
}

[[noreturn]] void usage(const char *error) {
    std::cerr << "harness: " << error << "\n"
              << "usage: harness --elf FILE --mode interp|aot --sentinel HEX "
                 "--sp HEX [--mem ADDR=HEXBYTES] [--digest START:END] "
                 "[--trace HEX] [--seed N] [--skip-register] [--strict] --trace-out FILE\n";
    std::exit(2);
}

std::uint32_t parse_hex(const std::string &text) {
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
}

std::vector<std::uint8_t> parse_hexbytes(const std::string &text) {
    if (text.size() % 2u != 0u) usage("--mem payload must be full hex bytes");
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2u);
    for (std::size_t i = 0; i < text.size(); i += 2u)
        out.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return out;
}

} // namespace

int main(int argc, char **argv) {
    std::string elf_path, mode, trace_path;
    std::uint32_t sentinel_vaddr = 0u, sp_vaddr = 0u;
    std::uint64_t seed = 0u;
    bool skip_register = false;
    bool strict = false;
    struct MemInit { std::uint32_t addr; std::string hex; };
    std::vector<MemInit> memory;
    std::vector<Range> digest_ranges;
    std::vector<std::uint32_t> trace_vaddrs;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) usage((std::string(name) + " needs a value").c_str());
            return argv[++i];
        };
        if (arg == "--elf") elf_path = next("--elf");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--sentinel") sentinel_vaddr = parse_hex(next("--sentinel"));
        else if (arg == "--sp") sp_vaddr = parse_hex(next("--sp"));
        else if (arg == "--mem") {
            const std::string value = next("--mem");
            const auto eq = value.find('=');
            if (eq == std::string::npos) usage("--mem needs ADDR=HEXBYTES");
            memory.push_back({parse_hex(value.substr(0, eq)), value.substr(eq + 1)});
        } else if (arg == "--digest") {
            const std::string value = next("--digest");
            const auto colon = value.find(':');
            if (colon == std::string::npos) usage("--digest needs START:END");
            digest_ranges.push_back(
                {parse_hex(value.substr(0, colon)), parse_hex(value.substr(colon + 1))});
        } else if (arg == "--trace") {
            trace_vaddrs.push_back(parse_hex(next("--trace")));
        } else if (arg == "--seed") {
            seed = std::stoull(next("--seed"));
        } else if (arg == "--skip-register") {
            skip_register = true;
        } else if (arg == "--strict") {
            strict = true;
        } else if (arg == "--trace-out") {
            trace_path = next("--trace-out");
        } else {
            usage(("unknown argument: " + arg).c_str());
        }
    }
    if (elf_path.empty() || (mode != "interp" && mode != "aot") || trace_path.empty())
        usage("--elf, --mode and --trace-out are required");

    Harness harness;
    g_harness = &harness;
    harness.trace.open(trace_path, std::ios::binary);
    if (!harness.trace) usage("cannot open trace output");

    psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
    harness.load_base = psprecomp::kDefaultPspUserLoadBase;
    elf.load_and_relocate(harness.runtime.memory(), harness.load_base);

    for (const MemInit &init : memory)
        harness.runtime.memory().copy_in(runtime_address(harness, init.addr),
                                         parse_hexbytes(init.hex));
    for (const Range &range : digest_ranges)
        harness.digest_ranges.push_back(
            {runtime_address(harness, range.start), runtime_address(harness, range.end)});
    for (const std::uint32_t vaddr : trace_vaddrs)
        harness.trace_pcs.insert(runtime_address(harness, vaddr));
    harness.trace_pcs.insert(runtime_address(harness, sentinel_vaddr));

    // TC-4.4: deterministic randomized VFPU state, identical in both tiers.
    if (seed != 0u) {
        std::uint64_t state = seed * 0x9E3779B97F4A7C15ull;
        auto next_random = [&state]() {
            state += 0x9E3779B97F4A7C15ull;
            std::uint64_t z = state;
            z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31u);
        };
        for (float &value : harness.runtime.cpu().vfpu) {
            const std::uint64_t bits = next_random();
            float magnitude = 0.25f + static_cast<float>(bits & 0x3FFu) / 512.0f;
            if ((bits >> 63u) != 0u) magnitude = -magnitude;
            value = magnitude;
        }
    }
    harness.runtime.cpu().gpr[29] = runtime_address(harness, sp_vaddr);
    harness.runtime.cpu().gpr[31] = runtime_address(harness, sentinel_vaddr);
    harness.runtime.register_function(runtime_address(harness, sentinel_vaddr),
                                      sentinel, "fixture-sentinel");

    if (mode == "aot" && !skip_register)
        psprecomp::register_generated_functions(harness.runtime);

    psprecomp::set_runtime_pre_dispatch_hook(&pre_dispatch);
    if (strict) psprecomp::set_interpreter_fallback_enabled(false);

    const std::uint32_t entry = elf.entry();
    harness.trace << "entry " << psprecomp::hex32(entry) << "\n";
    record("start", entry);
    harness.runtime.run(entry, 100000u);

    const std::string status = harness.runtime.stopped() ? "stopped" : "budget";
    harness.trace << "end " << status << " " << psprecomp::hex32(harness.runtime.cpu().pc) << " "
                  << digest(harness.runtime) << "\n";
    const AllegrexContext &final_cpu = harness.runtime.cpu();
    harness.trace << "final-regs";
    for (std::uint32_t i = 0; i < 32; ++i)
        harness.trace << " r" << i << "=" << psprecomp::hex32(final_cpu.gpr[i]);
    harness.trace << " hi=" << psprecomp::hex32(final_cpu.hi)
                  << " lo=" << psprecomp::hex32(final_cpu.lo) << "\n";
    for (const Range &range : harness.digest_ranges) {
        std::vector<std::uint8_t> window(range.end - range.start);
        harness.runtime.memory().copy_out(range.start, window);
        harness.trace << "final-mem " << psprecomp::hex32(range.start) << " ";
        for (const std::uint8_t byte : window) {
            static const char digits[] = "0123456789abcdef";
            harness.trace << digits[byte >> 4u] << digits[byte & 0xFu];
        }
        harness.trace << "\n";
    }
    if (harness.runtime.stopped()) {
        const std::string reason = harness.runtime.stop_reason();
        harness.trace << "reason " << reason << "\n";
        harness.trace.close();
        // 0 = clean fixture completion; 4 = loud stop (edge behaviour under test).
        return reason == "fixture-done" ? 0 : 4;
    }
    harness.trace.close();
    return 3; // dispatch budget exhausted: never silent success
}

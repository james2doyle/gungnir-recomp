// Bounded dispatch-coverage harness (plan T5, TC-5.4).
//
// Loads a real module with the generated AOT corpus registered and runs from
// the module entry for a bounded number of dispatches. Every outer dispatch is
// classified as translated (a registered function ran) or interpreter fallback
// (the permanent interpreter tier ran because no function claimed the PC);
// fallback addresses are listed explicitly. Chaining is disabled by the
// dispatch hooks, so each classification is a real outer dispatch.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

struct Coverage {
    std::uint64_t translated{};
    std::uint64_t interpreter{};
    std::set<std::uint32_t> interpreter_addresses;
    std::vector<std::pair<char, std::uint32_t>> first_dispatches;
};

Coverage *g_coverage = nullptr;

void record_first(char kind, std::uint32_t pc) {
    if (g_coverage->first_dispatches.size() < 12u)
        g_coverage->first_dispatches.emplace_back(kind, pc);
}

void pre_dispatch(Runtime &rt, AllegrexContext &, std::uint32_t pc, std::int32_t) {
    ++g_coverage->translated;
    record_first(rt.has_function(pc) ? 'T' : '?', pc);
}

bool missing_function(Runtime &, AllegrexContext &, std::uint32_t pc) {
    ++g_coverage->interpreter;
    g_coverage->interpreter_addresses.insert(pc);
    record_first('I', pc);
    return false; // let the permanent interpreter tier handle it
}

[[noreturn]] void usage(const char *message) {
    std::cerr << "game_coverage: " << message << "\n"
              << "usage: game_coverage --elf FILE --load-base HEX --entry HEX\n"
                 "                     --max-dispatches N --unit-span HEX\n"
                 "                     --trace-out FILE [--stack-top HEX]\n"
                 "                     [--drop-unit N] [--drop-entry-unit]\n";
    std::exit(2);
}

std::uint32_t parse_hex(const std::string &text) {
    return static_cast<std::uint32_t>(std::stoul(text, nullptr, 0));
}

std::string hex32(std::uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
        out += digits[(value >> static_cast<unsigned>(shift)) & 0xFu];
    return out;
}

} // namespace

int main(int argc, char **argv) {
    std::string elf_path, trace_path;
    std::uint32_t load_base = 0u, entry = 0u, unit_span = 0x4000u;
    std::uint32_t stack_top = 0u;
    std::uint64_t max_dispatches = 100000u;
    std::vector<std::uint32_t> drop_units;
    bool drop_entry_unit = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) usage((std::string(name) + " needs a value").c_str());
            return argv[++i];
        };
        if (arg == "--elf") elf_path = next("--elf");
        else if (arg == "--load-base") load_base = parse_hex(next("--load-base"));
        else if (arg == "--entry") entry = parse_hex(next("--entry"));
        else if (arg == "--max-dispatches") max_dispatches = std::stoull(next("--max-dispatches"));
        else if (arg == "--unit-span") unit_span = parse_hex(next("--unit-span"));
        else if (arg == "--stack-top") stack_top = parse_hex(next("--stack-top"));
        else if (arg == "--trace-out") trace_path = next("--trace-out");
        else if (arg == "--drop-unit") drop_units.push_back(
            static_cast<std::uint32_t>(std::stoul(next("--drop-unit"))));
        else if (arg == "--drop-entry-unit") drop_entry_unit = true;
        else usage(("unknown argument: " + arg).c_str());
    }
    if (elf_path.empty() || trace_path.empty() || load_base == 0u || entry == 0u)
        usage("--elf, --load-base, --entry and --trace-out are required");
    if (unit_span == 0u) usage("--unit-span must be non-zero");

    Runtime runtime;
    psprecomp::Elf32Image elf = psprecomp::Elf32Image::from_file(elf_path);
    (void)elf.load_and_relocate(runtime.memory(), load_base);
    psprecomp::register_generated_functions(runtime);

    // The PSP loader gives the module a stack; a raw entry call with $sp = 0
    // would fault in the prologue before any translated work is observable.
    if (stack_top == 0u) stack_top = 0x09FFF000u;
    runtime.cpu().gpr[29] = stack_top;

    if (drop_entry_unit)
        drop_units.push_back((entry - load_base) / unit_span);
    for (const std::uint32_t unit : drop_units) {
        const std::uint32_t start = load_base + unit * unit_span;
        runtime.unregister_functions(start, start + unit_span);
    }
    const bool entry_registered = runtime.has_function(entry);

    Coverage coverage;
    g_coverage = &coverage;
    psprecomp::set_runtime_pre_dispatch_hook(&pre_dispatch);
    psprecomp::set_runtime_missing_function_hook(&missing_function);

    runtime.run(entry, max_dispatches);

    std::ofstream out(trace_path);
    if (!out) usage("cannot open trace output");
    out << "{\n"
        << "  \"entry\": \"" << hex32(entry) << "\",\n"
        << "  \"dispatches\": " << coverage.translated + coverage.interpreter << ",\n"
        << "  \"translated\": " << coverage.translated << ",\n"
        << "  \"interpreter\": " << coverage.interpreter << ",\n"
        << "  \"interpreter_addresses\": [";
    bool first = true;
    for (const std::uint32_t address : coverage.interpreter_addresses) {
        if (!first) out << ", ";
        out << "\"" << hex32(address) << "\"";
        first = false;
    }
    out << "],\n"
        << "  \"first_dispatches\": [";
    first = true;
    for (const auto &[kind, pc] : coverage.first_dispatches) {
        if (!first) out << ", ";
        out << "\"" << kind << " " << hex32(pc) << "\"";
        first = false;
    }
    out << "],\n"
        << "  \"dispatcher_work\": " << runtime.dispatch_work_count() << ",\n"
        << "  \"entry_registered\": " << (entry_registered ? "true" : "false") << ",\n"
        << "  \"drop_units\": " << drop_units.size() << ",\n"
        << "  \"stopped\": " << (runtime.stopped() ? "true" : "false") << ",\n"
        << "  \"stop_reason\": \"";
    for (const char c : runtime.stop_reason()) {
        if (c == '"' || c == '\\') out << '\\';
        out << c;
    }
    out << "\",\n"
        << "  \"stop_pc\": \"" << hex32(runtime.cpu().pc) << "\"\n"
        << "}\n";
    return 0;
}

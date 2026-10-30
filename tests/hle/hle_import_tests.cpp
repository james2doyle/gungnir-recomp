// T6a fixture: HLE registrar and fail-closed unknown imports.
//
// Usage: hle_import_tests --nids <csv> --csv | --registrar | --unknown
// Exit 0 on the asserted contract, 1 on a failed contract, 2 on usage.

#include "hle/hle_common.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using gungnir::hle::HleRegistrar;
using psprecomp::AllegrexContext;
using psprecomp::Runtime;

[[noreturn]] void usage(const char *message) {
    std::cerr << "hle_import_tests: " << message
              << "\nusage: hle_import_tests --nids CSV --csv|--registrar|--unknown\n";
    std::exit(2);
}

int fail(const std::string &message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

int run_csv(Runtime &runtime) {
    const auto name = runtime.nids().resolve("Kernel_Library", 0x293B45B8u);
    if (!name) return fail("Kernel_Library:0x293B45B8 did not resolve");
    std::cout << "Kernel_Library:0x293B45B8=" << *name << "\n";
    if (*name != "sceKernelGetThreadId") return fail("unexpected alias name " + *name);
    return 0;
}

int run_registrar(Runtime &runtime) {
    HleRegistrar hle(runtime);
    int calls = 0;
    if (!hle.try_add("Kernel_Library", "sceKernelGetThreadId",
            [&calls](Runtime &, AllegrexContext &ctx) {
                ++calls;
                ctx.gpr[2] = 0x1234u;
            }))
        return fail("registrar could not bind cross-library alias");
    if (!hle.bound("Kernel_Library", 0x293B45B8u)) return fail("bound() did not record the alias");
    AllegrexContext &ctx = runtime.cpu();
    runtime.invoke_import("Kernel_Library", 0x293B45B8u, ctx);
    if (runtime.stopped()) return fail("bound import stopped: " + runtime.stop_reason());
    if (calls != 1) return fail("handler call count " + std::to_string(calls));
    if (ctx.gpr[2] != 0x1234u) return fail("handler did not set v0");
    std::cout << "registrar: bound and invoked alias\n";
    return 0;
}

int run_unknown(Runtime &runtime) {
    HleRegistrar hle(runtime);
    AllegrexContext &ctx = runtime.cpu();
    runtime.invoke_import("TestLibrary", 0xDEADBEEFu, ctx);
    if (!runtime.stopped()) return fail("unknown import did not stop");
    const std::string reason = runtime.stop_reason();
    std::cout << "stopped: " << reason << "\n";
    for (const char *needle : {"Missing HLE import", "TestLibrary", "NID_0xDEADBEEF"}) {
        if (reason.find(needle) == std::string::npos)
            return fail(std::string("reason does not name ") + needle + ": " + reason);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    std::string nids;
    std::string mode;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--nids" && i + 1 < argc) nids = argv[++i];
        else if (arg == "--csv" || arg == "--registrar" || arg == "--unknown") mode = arg;
        else usage(("unknown argument: " + arg).c_str());
    }
    if (nids.empty() || mode.empty()) usage("--nids and one mode are required");

    try {
        Runtime runtime;
        gungnir::hle::load_nid_registry(runtime, nids);
        if (mode == "--csv") return run_csv(runtime);
        if (mode == "--registrar") return run_registrar(runtime);
        return run_unknown(runtime);
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

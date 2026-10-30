// T6d fixtures: module registry, stop/unload and corpus reload semantics.
//
// Usage: hle_module_tests --nids CSV
// Exit 0 on success, 1 on a failed contract, 2 on usage.

#include "hle/hle_common.hpp"
#include "hle/hle_module.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

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

void corpus_a(Runtime &, AllegrexContext &ctx) {
    ctx.pc = 0x08804000u;
}

void corpus_b(Runtime &, AllegrexContext &ctx) {
    ctx.pc = 0x08804000u;
}

void install_corpus_a(Runtime &runtime) {
    runtime.register_function(0x08830000u, &corpus_a, "overlay-a");
}

void install_corpus_b(Runtime &runtime) {
    runtime.register_function(0x08830004u, &corpus_b, "overlay-b");
}

class Fixture {
public:
    static constexpr std::uint32_t kText = 0x08810000u;
    static constexpr std::uint32_t kOutput = 0x08811000u;
    static constexpr std::uint32_t kReturnAddress = 0x08822000u;
    static constexpr std::uint32_t kOverlayBase = 0x08830000u;
    static constexpr std::uint32_t kOverlaySize = 0x1000u;

    explicit Fixture(const std::filesystem::path &nids) : runtime(32u * 1024u * 1024u) {
        gungnir::hle::reset_modules();
        kernel() = Kernel{};
        kernel().install(runtime, 0x08801000u, 0x08820000u);
        kernel().set_pacing_enabled(false);
        runtime.nids().load_csv(nids);
        for (const auto &symbol : runtime.nids().all())
            nids_by_name[{symbol.library, symbol.name}] = symbol.nid;
        HleRegistrar hle(runtime);
        gungnir::hle::register_modulemgr(hle);
        gungnir::hle::register_main_module("9th", 0x08804000u, 0x00010000u, 0x08801234u);
        kernel().start_loader_thread(ctx, 0x08820000u, 0u);
    }

    ~Fixture() {
        gungnir::hle::reset_modules();
        kernel() = Kernel{};
        psprecomp::set_runtime_starvation_hook(nullptr, 0);
    }

    std::uint32_t call(const std::string &library, const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        for (unsigned i = 0; i < 8u; ++i) ctx.set_gpr(i + 4u, 0u);
        unsigned index = 0;
        for (const std::uint32_t value : args) ctx.set_gpr(4u + index++, value);
        ctx.set_gpr(31, kReturnAddress);
        const auto found = nids_by_name.find({library, name});
        if (found == nids_by_name.end())
            throw std::runtime_error("no NID for " + library + "::" + name);
        runtime.invoke_import(library, found->second, ctx);
        return ctx.gpr[2];
    }

    std::uint32_t module(const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        return call("ModuleMgrForUser", name, args);
    }

    Runtime runtime;
    Kernel &k = kernel();
    AllegrexContext &ctx = runtime.cpu();
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name;
};

void lookup_contracts() {
    Fixture f(test_nids_path());
    check(f.module("sceKernelGetModuleId") == gungnir::hle::kMainModuleId,
          "current module id is the main module");
    check(f.module("sceKernelGetModuleIdByAddress", {0x08804500u}) == gungnir::hle::kMainModuleId,
          "address inside the main module resolves");
    check(static_cast<std::uint32_t>(f.module("sceKernelGetModuleIdByAddress", {0x08100000u})) ==
              gungnir::hle::error::kUnknownModule,
          "address outside every module reports unknown");
    check(gungnir::hle::module_count() == 1u, "one module registered");
}

void reload_contracts() {
    Fixture f(test_nids_path());
    const std::size_t baseline = f.runtime.function_count();

    const auto first = static_cast<std::uint32_t>(gungnir::hle::add_module(f.runtime, "overlay-a",
        Fixture::kOverlayBase, Fixture::kOverlaySize, Fixture::kOverlayBase, &install_corpus_a));
    check(static_cast<std::int32_t>(first) > 0, "overlay module registered");
    check(first != gungnir::hle::kMainModuleId, "overlay id differs from the main module");
    check(f.runtime.has_function(Fixture::kOverlayBase), "corpus A registered at its slot");
    check(f.runtime.function_count() == baseline + 1u, "one corpus function registered");
    check(f.module("sceKernelGetModuleIdByAddress", {Fixture::kOverlayBase + 8u}) == first,
          "overlay address resolves to the overlay id");

    check(f.module("sceKernelStopModule", {first, 0u, 0u, Fixture::kOutput, 0u}) == 0u,
          "stop succeeds");
    check(f.runtime.memory().load32(Fixture::kOutput) == 0u, "stop writes module status");
    check(f.runtime.has_function(Fixture::kOverlayBase), "stop keeps the corpus");
    check(f.module("sceKernelUnloadModule", {first}) == 0u, "unload succeeds");
    check(!f.runtime.has_function(Fixture::kOverlayBase), "unload removes the corpus");
    check(f.runtime.function_count() == baseline, "unload leaks no registrations");
    check(static_cast<std::uint32_t>(f.module("sceKernelGetModuleIdByAddress", {Fixture::kOverlayBase})) ==
              gungnir::hle::error::kUnknownModule,
          "unloaded module no longer covers its slot");
    check(static_cast<std::uint32_t>(f.module("sceKernelUnloadModule", {first})) ==
              gungnir::hle::error::kUnknownModule,
          "double unload reports unknown module");

    const auto second = static_cast<std::uint32_t>(gungnir::hle::add_module(f.runtime, "overlay-b",
        Fixture::kOverlayBase, Fixture::kOverlaySize, Fixture::kOverlayBase, &install_corpus_b));
    check(second != first, "reload gets a fresh module id");
    check(f.runtime.has_function(Fixture::kOverlayBase + 4u), "corpus B registered in the reused slot");
    check(!f.runtime.has_function(Fixture::kOverlayBase), "the previous corpus is gone");
    check(f.runtime.function_count() == baseline + 1u, "reload replaces the previous corpus");
    check(f.module("sceKernelUnloadModule", {second}) == 0u, "reloaded module unloads");
    check(f.runtime.function_count() == baseline, "no registrations survive the reload cycle");
}

void guard_contracts() {
    Fixture f(test_nids_path());
    check(static_cast<std::uint32_t>(f.module("sceKernelUnloadModule", {gungnir::hle::kMainModuleId})) ==
              gungnir::hle::error::kIllegalPerm,
          "the main module cannot be unloaded");
    check(static_cast<std::uint32_t>(f.module("sceKernelUnloadModule", {0x7FFFFFFFu})) ==
              gungnir::hle::error::kUnknownModule,
          "unknown module id is named");
    check(gungnir::hle::module_count() == 1u, "guards leave the registry intact");
}

} // namespace

int main(int argc, char **argv) {
    std::string nids;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--nids" && i + 1 < argc) nids = argv[++i];
        else {
            std::cerr << "usage: hle_module_tests --nids CSV\n";
            return 2;
        }
    }
    if (nids.empty()) return 2;
    setenv("GUNGNIR_TEST_NIDS_CSV", nids.c_str(), 1);
    try {
        lookup_contracts();
        reload_contracts();
        guard_contracts();
        std::cout << "hle_module_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

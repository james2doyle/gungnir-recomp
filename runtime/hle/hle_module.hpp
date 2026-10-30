#pragma once

// Module registry and ModuleMgrForUser stop/unload semantics (T6d).
//
// This title is a single-module executable: it imports only the lookup,
// stop and unload calls, and the disc ships no PRX files. The registry still
// tracks every module and its AOT address window so a precompiled corpus can
// be installed on load and removed on unload without leaking dispatch
// registrations. The loader itself (`sceKernelLoadModule`) is deliberately
// absent until a title that imports it needs one; an executed load import
// therefore stops loudly.

#include "hle/hle_common.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace gungnir::hle {

inline constexpr SceUID kMainModuleId = 0x01000001;

struct Module {
    std::string name;
    SceUID id{};
    std::uint32_t base{};
    std::uint32_t size{};
    std::uint32_t gp{};
    bool main{};
    bool started{true};
    // Registers the module's precompiled corpus in its address window. Empty
    // for the main module, whose corpus is registered by the boot runner.
    std::function<void(Runtime &)> install_corpus;
};

// Test and boot-runner lifecycle. The process starts with no modules.
void reset_modules();
void register_main_module(std::string name, std::uint32_t base, std::uint32_t size, std::uint32_t gp);

// Installs a module record and, when given, its corpus. This is the
// translate-on-load seam: AOT translation happens offline in T5, so the
// corpus callback only registers the functions for the module's window.
SceUID add_module(Runtime &runtime, std::string name, std::uint32_t base, std::uint32_t size,
    std::uint32_t gp, std::function<void(Runtime &)> install_corpus = {});

[[nodiscard]] const Module *find_module(SceUID id);
[[nodiscard]] const Module *module_for_address(std::uint32_t address);
[[nodiscard]] std::size_t module_count();

void register_modulemgr(HleRegistrar &hle);

} // namespace gungnir::hle

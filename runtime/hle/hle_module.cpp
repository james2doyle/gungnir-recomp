#include "hle/hle_module.hpp"

#include "hle/kernel.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace gungnir::hle {
namespace {

std::map<SceUID, Module> &modules() {
    static std::map<SceUID, Module> registry;
    return registry;
}

SceUID &next_module_id() {
    static SceUID next = kMainModuleId + 1;
    return next;
}

SceUID current_module_id() {
    return modules().contains(kMainModuleId) ? kMainModuleId : 0;
}

void unregister_corpus(Runtime &runtime, const Module &module) {
    runtime.unregister_functions(module.base, module.base + module.size);
}

} // namespace

void reset_modules() {
    modules().clear();
    next_module_id() = kMainModuleId + 1;
}

void register_main_module(std::string name, std::uint32_t base, std::uint32_t size, std::uint32_t gp) {
    Module module{};
    module.name = std::move(name);
    module.id = kMainModuleId;
    module.base = base;
    module.size = size;
    module.gp = gp;
    module.main = true;
    module.started = true;
    modules()[module.id] = std::move(module);
}

SceUID add_module(Runtime &runtime, std::string name, std::uint32_t base, std::uint32_t size,
    std::uint32_t gp, std::function<void(Runtime &)> install_corpus) {
    if (size == 0u || (base & 3u) != 0u) return static_cast<SceUID>(error::kIllegalArgument);
    if (module_for_address(base) != nullptr) return static_cast<SceUID>(error::kIllegalArgument);
    Module module{};
    module.name = std::move(name);
    module.id = next_module_id()++;
    module.base = base;
    module.size = size;
    module.gp = gp;
    module.install_corpus = std::move(install_corpus);
    modules()[module.id] = std::move(module);
    if (const auto found = modules().find(module.id); found != modules().end() && found->second.install_corpus) {
        try {
            found->second.install_corpus(runtime);
        } catch (...) {
            modules().erase(found);
            throw;
        }
    }
    return module.id;
}

const Module *find_module(SceUID id) {
    const auto found = modules().find(id);
    return found != modules().end() ? &found->second : nullptr;
}

const Module *module_for_address(std::uint32_t address) {
    for (const auto &[id, module] : modules()) {
        (void)id;
        if (address >= module.base && static_cast<std::uint64_t>(address) <
                static_cast<std::uint64_t>(module.base) + module.size)
            return &module;
    }
    return nullptr;
}

std::size_t module_count() {
    return modules().size();
}

void register_modulemgr(HleRegistrar &hle) {
    hle.add("ModuleMgrForUser", "sceKernelGetModuleId", [](Runtime &, AllegrexContext &ctx) {
        const SceUID id = current_module_id();
        kernel().finish(ctx, id != 0 ? static_cast<std::uint32_t>(id) : error::kUnknownModule);
    });
    hle.add("ModuleMgrForUser", "sceKernelGetModuleIdByAddress", [](Runtime &, AllegrexContext &ctx) {
        const Module *module = module_for_address(arg(ctx, 0));
        kernel().finish(ctx, module != nullptr ? static_cast<std::uint32_t>(module->id) : error::kUnknownModule);
    });
    hle.add("ModuleMgrForUser", "sceKernelStopModule", [](Runtime &rt, AllegrexContext &ctx) {
        const auto id = static_cast<SceUID>(arg(ctx, 0));
        const auto found = modules().find(id);
        if (found == modules().end()) {
            kernel().finish(ctx, error::kUnknownModule);
            return;
        }
        if (!found->second.started) {
            kernel().finish(ctx, error::kAlreadyStopped);
            return;
        }
        found->second.started = false;
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
        kernel().finish(ctx, 0u);
    });
    hle.add("ModuleMgrForUser", "sceKernelUnloadModule", [](Runtime &rt, AllegrexContext &ctx) {
        const auto id = static_cast<SceUID>(arg(ctx, 0));
        const auto found = modules().find(id);
        if (found == modules().end()) {
            kernel().finish(ctx, error::kUnknownModule);
            return;
        }
        if (found->second.main) {
            kernel().finish(ctx, error::kIllegalPerm);
            return;
        }
        // Drop the module's dispatch window before the address space can be
        // reused; a reload installs its own corpus there.
        unregister_corpus(rt, found->second);
        modules().erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ModuleMgrForUser", "sceKernelStopUnloadSelfModuleWithStatus",
        [](Runtime &rt, AllegrexContext &ctx) {
            const auto exit_code = static_cast<std::int32_t>(arg(ctx, 0));
            if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
            Thread *thread = kernel().current_thread();
            const Module *module = thread != nullptr ? module_for_address(thread->entry) : nullptr;
            if (module != nullptr && !module->main) {
                unregister_corpus(rt, *module);
                modules().erase(module->id);
            }
            kernel().exit_current_thread(ctx, exit_code, true);
        });
}

} // namespace gungnir::hle

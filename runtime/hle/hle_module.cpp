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

namespace {

void pput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

struct PReader {
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

    [[nodiscard]] std::string str() {
        const std::uint32_t len = u32();
        if (!in_ok(len)) {
            ok = false;
            return {};
        }
        std::string text(reinterpret_cast<const char *>(p), len);
        p += len;
        left -= len;
        return text;
    }

    [[nodiscard]] bool in_ok(std::uint32_t len) const { return ok && left >= len && len <= 256u; }
};

} // namespace

bool export_snapshot_modules(std::vector<std::uint8_t> &bytes_out, std::string &error) {
    bytes_out.clear();
    pput_u32(bytes_out, static_cast<std::uint32_t>(next_module_id()));
    pput_u32(bytes_out, static_cast<std::uint32_t>(modules().size()));
    for (const auto &[id, module] : modules()) {
        pput_u32(bytes_out, static_cast<std::uint32_t>(id));
        pput_u32(bytes_out, static_cast<std::uint32_t>(module.name.size()));
        bytes_out.insert(bytes_out.end(), module.name.begin(), module.name.end());
        pput_u32(bytes_out, module.base);
        pput_u32(bytes_out, module.size);
        pput_u32(bytes_out, module.gp);
        bytes_out.push_back(module.main ? 1u : 0u);
        bytes_out.push_back(module.started ? 1u : 0u);
    }
    error.clear();
    return true;
}

bool import_snapshot_modules(std::span<const std::uint8_t> bytes, std::string &error) {
    PReader in{bytes.data(), bytes.size()};
    const SceUID next = static_cast<SceUID>(in.u32());
    const std::uint32_t count = in.u32();
    if (!in.ok || count > 64u) {
        error = "snapshot_truncated: module header malformed";
        return false;
    }
    std::map<SceUID, Module> staged;
    for (std::uint32_t i = 0; i < count; ++i) {
        const SceUID id = static_cast<SceUID>(in.u32());
        Module module;
        module.id = id;
        module.name = in.str();
        module.base = in.u32();
        module.size = in.u32();
        module.gp = in.u32();
        const std::uint8_t main = in.u8();
        const std::uint8_t started = in.u8();
        if (!in.ok || main > 1u || started > 1u || module.size == 0u || (module.base & 3u) != 0u) {
            error = "snapshot_bad_length: module record malformed";
            return false;
        }
        module.main = main != 0u;
        module.started = started != 0u;
        if (!staged.emplace(id, std::move(module)).second) {
            error = "snapshot_duplicate_uid: duplicate module id=" + std::to_string(id);
            return false;
        }
    }
    if (!in.ok || in.left != 0u) {
        error = "snapshot_truncated: trailing bytes in module section";
        return false;
    }
    for (const auto &[id, module] : staged) {
        if (next <= id) {
            error = "snapshot_duplicate_uid: next module id reuses live id";
            return false;
        }
        for (const auto &[other_id, other] : staged) {
            if (other_id != id && module.base < other.base + other.size &&
                other.base < module.base + module.size) {
                error = "snapshot_bad_length: module windows overlap";
                return false;
            }
        }
    }
    modules().clear();
    for (auto &[id, module] : staged) modules().emplace(id, std::move(module));
    next_module_id() = next;
    error.clear();
    return true;
}

} // namespace gungnir::hle

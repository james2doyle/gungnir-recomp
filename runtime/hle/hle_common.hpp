#pragma once

// Shared HLE glue: PSP EABI argument access, guest string helpers, and the
// registrar that binds handlers by (library, symbol name).
//
// Adapted from TeamGDB/Yakumo's MIT `profiles/mhp3rd/host/hle/hle_common.*`
// at the pinned commit recorded in third_party/yakumo/PROVENANCE.md; the
// namespace and the profile-specific pieces (mods, media state, renderer)
// are removed. Behaviour: bind only known symbols, fail loudly otherwise.

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gungnir::hle {

using HleFunction = std::function<void(psprecomp::Runtime &, psprecomp::AllegrexContext &)>;

// PSP EABI argument registers: a0-a3 then t0-t3.
[[nodiscard]] inline std::uint32_t arg(const psprecomp::AllegrexContext &ctx, unsigned index) noexcept {
    return index < 4u ? ctx.gpr[4u + index] : ctx.gpr[8u + (index - 4u)];
}

// 64-bit arguments occupy an even-aligned register pair, low word first.
[[nodiscard]] inline std::uint64_t arg64(const psprecomp::AllegrexContext &ctx, unsigned low_index) noexcept {
    return static_cast<std::uint64_t>(arg(ctx, low_index)) |
        (static_cast<std::uint64_t>(arg(ctx, low_index + 1u)) << 32u);
}

[[nodiscard]] std::string read_cstring(
    const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t max_length = 512u);
void write_cstring(psprecomp::GuestMemory &memory, std::uint32_t address, std::string_view text, std::size_t capacity);
void store64(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t value);

// Loads the generated PSPSDK NID CSV into the runtime registry. Existing
// built-in entries stay; CSV rows fill the rest. Missing/unreadable files are
// a named error, never a silently empty registry.
void load_nid_registry(psprecomp::Runtime &runtime, const std::filesystem::path &csv);

class HleRegistrar {
public:
    explicit HleRegistrar(psprecomp::Runtime &runtime);

    // Resolves `name` in `library` and registers the handler. Returns false
    // when the name is unknown; throws when the name maps to conflicting NIDs.
    bool try_add(std::string_view library, std::string_view name, HleFunction function);
    void add(std::string_view library, std::string_view name, HleFunction function);
    [[nodiscard]] bool bound(const std::string &library, std::uint32_t nid) const;
    [[nodiscard]] std::size_t count() const noexcept { return bound_.size(); }

private:
    psprecomp::Runtime &runtime_;
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name_;
    std::map<std::string, std::vector<std::pair<std::string, std::uint32_t>>> nids_by_symbol_;
    std::set<std::pair<std::string, std::uint32_t>> bound_;
};

} // namespace gungnir::hle

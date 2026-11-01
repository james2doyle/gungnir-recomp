#pragma once

// Shared HLE glue: PSP EABI argument access, guest string helpers, and the
// registrar that binds handlers by (library, symbol name).
//
// Adapted from TeamGDB/Yakumo's MIT `profiles/mhp3rd/host/hle/hle_common.*`
// at the pinned commit recorded in third_party/yakumo/PROVENANCE.md; the
// namespace and the profile-specific pieces (mods, media state, renderer)
// are removed. Behaviour: bind only known symbols, fail loudly otherwise.

#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gungnir::hle {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;
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

// Reads from an open guest file descriptor without moving its position.
// Returns the number of bytes read; 0 when the descriptor is unknown.
std::size_t read_open_file(std::uint32_t fd, std::uint64_t offset, std::uint8_t *output, std::size_t size);

// Host directory backing ms0: (set by register_io).
std::filesystem::path io_memory_stick_path();

// Kernel-object tracing: GUNGNIR_TRACE_SYNC gates one-line activity logs.
[[nodiscard]] bool trace_sync();
void log_sync(const std::string &message);
void log_once(const std::string &key, const std::string &message);

// Optional HLE call ring: GUNGNIR_HLE_RING=N keeps the last N import calls
// with their arguments and return registers. The session dumps it when a run
// stops on a fault, so a wrong return or a missing setup call is visible
// without a debugger. Unset (or 0) disables it.
void dump_hle_ring(std::ostream &out);

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

void register_sysmem(HleRegistrar &hle);
void register_threadman(HleRegistrar &hle);
void register_system(HleRegistrar &hle);
void register_io(HleRegistrar &hle, const std::filesystem::path &disc_image,
    const std::filesystem::path &memory_stick);

} // namespace gungnir::hle

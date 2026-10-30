#include "hle/hle_common.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <unordered_set>

namespace gungnir::hle {

std::string read_cstring(const psprecomp::GuestMemory &memory, std::uint32_t address, std::size_t max_length) {
    std::string text;
    if (address == 0u) return text;
    for (std::size_t i = 0; i < max_length; ++i) {
        const auto c = static_cast<char>(memory.load8(address + static_cast<std::uint32_t>(i)));
        if (c == '\0') break;
        text.push_back(c);
    }
    return text;
}

void write_cstring(psprecomp::GuestMemory &memory, std::uint32_t address, std::string_view text, std::size_t capacity) {
    if (address == 0u || capacity == 0u) return;
    const std::size_t count = std::min(text.size(), capacity - 1u);
    for (std::size_t i = 0; i < count; ++i)
        memory.store8(address + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    memory.store8(address + static_cast<std::uint32_t>(count), 0u);
}

void store64(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t value) {
    memory.store32(address, static_cast<std::uint32_t>(value));
    memory.store32(address + 4u, static_cast<std::uint32_t>(value >> 32u));
}

void load_nid_registry(psprecomp::Runtime &runtime, const std::filesystem::path &csv) {
    if (!std::filesystem::is_regular_file(csv))
        throw psprecomp::Error("NID registry CSV not found: " + csv.string());
    runtime.nids().load_csv(csv);
}

HleRegistrar::HleRegistrar(psprecomp::Runtime &runtime) : runtime_(runtime) {
    for (const auto &symbol : runtime.nids().all()) {
        nids_by_name_[{symbol.library, symbol.name}] = symbol.nid;
        nids_by_symbol_[symbol.name].push_back({symbol.library, symbol.nid});
    }
    for (auto &[name, entries] : nids_by_symbol_) {
        std::sort(entries.begin(), entries.end());
        entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    }
}

bool HleRegistrar::try_add(std::string_view library, std::string_view name, HleFunction function) {
    std::uint32_t nid = 0u;
    const auto exact = nids_by_name_.find({std::string(library), std::string(name)});
    if (exact != nids_by_name_.end()) {
        nid = exact->second;
    } else {
        // Commercial modules sometimes import a symbol through an alias
        // library (Kernel_Library for a ThreadMan routine, for example). If
        // the name exists under exactly one NID, bind it under the importing
        // library. Distinct NIDs for one name are an ambiguity we refuse to
        // guess about.
        const auto symbol = nids_by_symbol_.find(std::string(name));
        if (symbol == nids_by_symbol_.end()) return false;
        const auto &entries = symbol->second;
        const std::uint32_t first = entries.front().second;
        for (const auto &entry : entries) {
            if (entry.second != first)
                throw psprecomp::Error("Ambiguous HLE symbol " + std::string(library) + "::" +
                    std::string(name) + " maps to multiple NIDs");
        }
        nid = first;
    }
    runtime_.register_hle(std::string(library), nid, std::move(function));
    bound_.insert({std::string(library), nid});
    return true;
}

void HleRegistrar::add(std::string_view library, std::string_view name, HleFunction function) {
    // Optional handler: the profile registers the whole boot surface, while
    // this title imports a subset. A name absent from the CSV is simply not
    // bound; if the guest later imports it, the runtime's missing-import trap
    // names the NID. Required coverage is checked against the module's import
    // inventory, not against this registration list.
    (void)try_add(library, name, std::move(function));
}

bool trace_sync() {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_SYNC") != nullptr;
    return enabled;
}

void log_sync(const std::string &message) {
    static std::uint64_t remaining = [] {
        const char *limit = std::getenv("GUNGNIR_TRACE_SYNC_LIMIT");
        return limit != nullptr ? std::strtoull(limit, nullptr, 10) : 4000ull;
    }();
    if (remaining == 0u) return;
    --remaining;
    const Thread *thread = kernel().current_thread();
    std::cerr << "[sync] " << (thread != nullptr ? thread->name : "interrupt") << " " << message << "\n";
}

void log_once(const std::string &key, const std::string &message) {
    static std::unordered_set<std::string> seen;
    if (seen.insert(key).second) std::cerr << message << "\n";
}

bool HleRegistrar::bound(const std::string &library, std::uint32_t nid) const {
    return bound_.contains({library, nid});
}

} // namespace gungnir::hle

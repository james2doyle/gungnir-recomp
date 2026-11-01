#include "session/diagnostics.hpp"

#include <array>
#include <cstdio>
#include <deque>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace gungnir::session {
namespace {

struct ChainEntry {
    std::uint64_t sequence{};
    std::uint32_t target{};
    std::uint32_t depth{};
    std::uint32_t ra{};
    std::array<std::uint32_t, 4> args{};
    std::uint32_t v0{};
    std::uint32_t v1{};
    std::string thread;
};

std::size_t g_capacity = 0u;
std::uint64_t g_sequence = 0u;
std::deque<ChainEntry> g_completed;
std::vector<ChainEntry> g_active;

std::string hex32(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", value);
    return text;
}

void print_entry(std::ostream &out, const ChainEntry &entry, bool inflight) {
    out << "[chain-ring] #" << entry.sequence << " target=" << hex32(entry.target)
        << " depth=" << entry.depth << " ra=" << hex32(entry.ra) << " a0=" << hex32(entry.args[0])
        << " a1=" << hex32(entry.args[1]) << " a2=" << hex32(entry.args[2])
        << " a3=" << hex32(entry.args[3]) << " v0=" << hex32(entry.v0) << " v1=" << hex32(entry.v1)
        << " thread=" << entry.thread;
    if (inflight) out << " inflight=1";
    out << "\n";
}

} // namespace

void configure_chain_ring(std::size_t capacity) {
    g_capacity = capacity;
    g_completed.clear();
    g_active.clear();
}

std::size_t chain_ring_capacity() { return g_capacity; }

void chain_ring_enter(std::uint32_t target, std::uint32_t depth, std::uint32_t ra,
    std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, const char *thread) {
    if (g_capacity == 0u) return;
    ChainEntry entry;
    entry.sequence = ++g_sequence;
    entry.target = target;
    entry.depth = depth;
    entry.ra = ra;
    entry.args = {a0, a1, a2, a3};
    entry.thread = thread != nullptr ? thread : "";
    g_active.push_back(std::move(entry));
}

void chain_ring_leave(std::uint32_t target, std::uint32_t depth, std::uint32_t v0, std::uint32_t v1) {
    if (g_capacity == 0u || g_active.empty()) return;
    if (g_active.back().target != target || g_active.back().depth != depth) return;
    ChainEntry entry = std::move(g_active.back());
    g_active.pop_back();
    entry.v0 = v0;
    entry.v1 = v1;
    if (g_completed.size() >= g_capacity) g_completed.pop_front();
    g_completed.push_back(std::move(entry));
}

void dump_chain_ring(std::ostream &out) {
    if (g_completed.empty() && g_active.empty()) return;
    out << "[chain-ring] last " << g_completed.size() + g_active.size() << " chained calls\n";
    for (const ChainEntry &entry : g_completed) print_entry(out, entry, false);
    for (const ChainEntry &entry : g_active) print_entry(out, entry, true);
    g_completed.clear();
    g_active.clear();
}

} // namespace gungnir::session

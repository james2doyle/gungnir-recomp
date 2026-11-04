#include "hle/cadence.hpp"

#include <algorithm>
#include <mutex>

namespace gungnir::hle {
namespace {

std::mutex g_mutex;
CadenceCounters g_counters;
std::uint64_t g_last_flip_vblank{};
std::uint64_t g_last_wait_vblank{};
bool g_has_wait_wake{};

std::size_t bucket(std::uint32_t vblanks) {
    return std::min<std::size_t>(vblanks, kCadenceHistogram - 1u);
}

} // namespace

CadenceCounters cadence_snapshot() {
    std::lock_guard lock(g_mutex);
    return g_counters;
}

void reset_cadence() {
    std::lock_guard lock(g_mutex);
    g_counters = {};
    g_last_flip_vblank = 0u;
    g_last_wait_vblank = 0u;
    g_has_wait_wake = false;
}

void note_vblank_wait(std::uint32_t requested, std::uint64_t vblank_count) {
    std::lock_guard lock(g_mutex);
    ++g_counters.vblank_waits;
    ++g_counters.vblank_wait_counts[bucket(requested == 0u ? 1u : requested)];
    const std::uint64_t elapsed =
        vblank_count >= g_last_flip_vblank ? vblank_count - g_last_flip_vblank : 0u;
    ++g_counters.wait_after_flip[bucket(static_cast<std::uint32_t>(elapsed))];
}

void note_vblank_wait_wake(std::uint64_t vblank_count) {
    std::lock_guard lock(g_mutex);
    g_last_wait_vblank = vblank_count;
    g_has_wait_wake = true;
}

void note_delay(std::uint32_t microseconds) {
    std::lock_guard lock(g_mutex);
    ++g_counters.delay_calls;
    g_counters.delay_us_total += microseconds;
    g_counters.delay_us_max = std::max<std::uint64_t>(g_counters.delay_us_max, microseconds);
    const std::size_t index = microseconds < 1'000u ? 0u
        : microseconds < 10'000u                        ? 1u
        : microseconds < 25'000u                        ? 2u
                                                        : 3u;
    ++g_counters.delay_buckets[index];
}

void note_audio_block(std::uint32_t microseconds) {
    std::lock_guard lock(g_mutex);
    ++g_counters.audio_block_calls;
    g_counters.audio_block_us_total += microseconds;
}

void note_flip(std::span<const gungnir::ge::Draw> draws, std::uint64_t vblank_count) {
    std::lock_guard lock(g_mutex);
    std::uint64_t draw_3d = 0u;
    std::uint64_t draw_2d = 0u;
    std::uint64_t vertices = 0u;
    for (const gungnir::ge::Draw &draw : draws) {
        if (draw.state.through)
            ++draw_2d;
        else
            ++draw_3d;
        vertices += draw.vertices.size();
    }
    ++g_counters.flips;
    g_counters.draws_3d += draw_3d;
    g_counters.draws_2d += draw_2d;
    g_counters.vertices += vertices;
    const std::uint64_t elapsed =
        vblank_count >= g_last_flip_vblank ? vblank_count - g_last_flip_vblank : 0u;
    ++g_counters.flip_vblanks[bucket(static_cast<std::uint32_t>(elapsed))];
    if (g_has_wait_wake) {
        const std::uint64_t after = vblank_count >= g_last_wait_vblank
            ? vblank_count - g_last_wait_vblank
            : 0u;
        ++g_counters.flip_after_wait[bucket(static_cast<std::uint32_t>(after))];
    }
    g_last_flip_vblank = vblank_count;
}

std::string format_cadence(const CadenceCounters &counters) {
    std::string line = "cadence flips=" + std::to_string(counters.flips) +
        " waits=" + std::to_string(counters.vblank_waits) + " waithist=";
    for (std::size_t i = 0; i < counters.vblank_wait_counts.size(); ++i) {
        if (i != 0u) line += '/';
        line += std::to_string(counters.vblank_wait_counts[i]);
    }
    line += " delays=" + std::to_string(counters.delay_calls) +
        " delay_us=" + std::to_string(counters.delay_us_total) +
        " delay_max_us=" + std::to_string(counters.delay_us_max) + " delay_buckets=";
    for (std::size_t i = 0; i < counters.delay_buckets.size(); ++i) {
        if (i != 0u) line += '/';
        line += std::to_string(counters.delay_buckets[i]);
    }
    line += " audio_block=" + std::to_string(counters.audio_block_calls) +
        " audio_block_us=" + std::to_string(counters.audio_block_us_total) +
        " draws3d=" + std::to_string(counters.draws_3d) +
        " draws2d=" + std::to_string(counters.draws_2d) +
        " vertices=" + std::to_string(counters.vertices) + " flipvblank=";
    for (std::size_t i = 0; i < counters.flip_vblanks.size(); ++i) {
        if (i != 0u) line += '/';
        line += std::to_string(counters.flip_vblanks[i]);
    }
    line += " waitafterflip=";
    for (std::size_t i = 0; i < counters.wait_after_flip.size(); ++i) {
        if (i != 0u) line += '/';
        line += std::to_string(counters.wait_after_flip[i]);
    }
    line += " flipafterwait=";
    for (std::size_t i = 0; i < counters.flip_after_wait.size(); ++i) {
        if (i != 0u) line += '/';
        line += std::to_string(counters.flip_after_wait[i]);
    }
    return line;
}

} // namespace gungnir::hle

#pragma once

// Frame-producer instrumentation: what paces the guest and what one flip
// costs. Hosts read the same counters (the boot runner prints them at exit,
// the Android host logs deltas next to the audio stats), so a cadence claim
// always names the counter it came from.
//
// Nothing here is guest state: the counters are derived, never exported to a
// snapshot and never restored. Writers are on every guest thread; readers are
// on the host.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ge/state_machine.hpp"

namespace gungnir::hle {

inline constexpr std::size_t kCadenceHistogram = 16u;

struct CadenceCounters {
    // sceDisplayWaitVblankStart(Multi)(CB): calls and the requested vblank
    // count clamped to 15 (index 15 means "15 or more").
    std::uint64_t vblank_waits{};
    std::array<std::uint64_t, kCadenceHistogram> vblank_wait_counts{};
    // sceKernelDelayThread(CB): calls, total/maximum requested microseconds and
    // four buckets (<1 ms, 1-10 ms, 10-25 ms, >=25 ms).
    std::uint64_t delay_calls{};
    std::uint64_t delay_us_total{};
    std::uint64_t delay_us_max{};
    std::array<std::uint64_t, 4> delay_buckets{};
    // A blocking sceAudio submission that parked behind the mixer drain.
    std::uint64_t audio_block_calls{};
    std::uint64_t audio_block_us_total{};
    // Flips and the vblanks elapsed since the previous flip (clamped to 15).
    std::uint64_t flips{};
    std::array<std::uint64_t, kCadenceHistogram> flip_vblanks{};
    // The same interval split at the two edges of the frame loop: vblanks from
    // the last flip to the next vblank wait, and from that wait's wake to the
    // flip. A frame interval larger than the requested Multi count is one of
    // these two.
    std::array<std::uint64_t, kCadenceHistogram> wait_after_flip{};
    std::array<std::uint64_t, kCadenceHistogram> flip_after_wait{};
    // Draws handed to presentation by the last flip, classified by the GE
    // through bit: 3D candidates versus 2D/HUD screen-space draws.
    std::uint64_t draws_3d{};
    std::uint64_t draws_2d{};
    std::uint64_t vertices{};
};

// A consistent copy for reporting.
[[nodiscard]] CadenceCounters cadence_snapshot();
void reset_cadence();

void note_vblank_wait(std::uint32_t requested, std::uint64_t vblank_count);
void note_vblank_wait_wake(std::uint64_t vblank_count);
void note_delay(std::uint32_t microseconds);
void note_audio_block(std::uint32_t microseconds);
// `vblank_count` is the kernel's current vblank counter at the flip.
void note_flip(std::span<const gungnir::ge::Draw> draws, std::uint64_t vblank_count);

// One greppable line: `cadence flips=... vblanks=... waithist=a/b/...`.
[[nodiscard]] std::string format_cadence(const CadenceCounters &counters);

} // namespace gungnir::hle

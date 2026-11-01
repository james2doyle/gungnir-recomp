#pragma once

// Display, GE submission, controller and RTC-facing media HLE (T6c).
//
// The timing and callback semantics are adapted from TeamGDB/Yakumo's MIT
// `profiles/mhp3rd/host/hle/hle_media.cpp` at the pinned commit recorded in
// third_party/yakumo/PROVENANCE.md. The renderer and audio stay out of this
// profile until T12; pad state comes from runtime/input (T11a).

#include "hle/hle_common.hpp"

#include "ge/state_machine.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace gungnir::hle {

struct DisplayState {
    std::uint32_t mode{};
    std::uint32_t width{480u};
    std::uint32_t height{272u};
    std::uint32_t framebuffer{};
    std::uint32_t buffer_width{};
    std::uint32_t pixel_format{};
    std::uint64_t flips{};
};

[[nodiscard]] DisplayState &display_state();

// Clears every media-object table and the display state. Test fixtures call it
// between runs; the boot runner starts from a fresh process.
void reset_media_state();

// Called after a guest frame flip. The boot runner uses it to stop at the
// first sceDisplaySetFrameBuf; interactive hosts would present here.
void set_display_flip_observer(std::function<void()> observer);

// Receives the draws accumulated since the previous real flip. The callback
// runs on the guest thread and must not block.
void set_ge_frame_sink(std::function<void(const std::vector<gungnir::ge::Draw> &)> sink);
[[nodiscard]] std::uint64_t ge_frame_count();
[[nodiscard]] std::uint64_t ge_gap_count();

void register_media(HleRegistrar &hle);

} // namespace gungnir::hle

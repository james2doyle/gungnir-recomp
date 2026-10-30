#pragma once

// Display, GE submission, controller and RTC-facing media HLE (T6c).
//
// The timing and callback semantics are adapted from TeamGDB/Yakumo's MIT
// `profiles/mhp3rd/host/hle/hle_media.cpp` at the pinned commit recorded in
// third_party/yakumo/PROVENANCE.md. That file also contains the renderer,
// audio and input implementation; those stay out of this profile until T7/T12.

#include "hle/hle_common.hpp"

#include <cstdint>
#include <functional>

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

void register_media(HleRegistrar &hle);

} // namespace gungnir::hle

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
#include <span>
#include <string>
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

// Host widescreen widening factor for the GE pipeline (1 = native 480x272).
// Presentation policy: never guest state, never part of a snapshot. Applies to
// the live machine and to every machine rebuilt by a reset or a snapshot load.
void set_widescreen_scale(float scale);
float widescreen_scale();

// Draws translated since the last presentable flip. A flip hands them to the
// frame sink or drops them; this must not grow across flips.
[[nodiscard]] std::size_t ge_pending_draw_count();
[[nodiscard]] std::uint64_t ge_frame_count();
[[nodiscard]] std::uint64_t ge_gap_count();

void register_media(HleRegistrar &hle);

// Snapshot export/import (T15c). Saves display state, GE callbacks/lists
// (list runners are rebuilt from guest RAM on import), the GE state machine
// registers, input-latch state and frame counters. Completed draws awaiting
// presentation are derived: export fails with snapshot_busy when any are
// pending (flush GPU drawing before saving). Texture caches and gap-log
// dedup are dropped on import; sinks and observers are preserved. Import is
// fail-closed and needs the running Runtime to rebuild list runners.
[[nodiscard]] bool export_snapshot_media(std::vector<std::uint8_t> &bytes_out, std::string &error);
bool import_snapshot_media(
    std::span<const std::uint8_t> bytes, psprecomp::Runtime &runtime, std::string &error);

} // namespace gungnir::hle

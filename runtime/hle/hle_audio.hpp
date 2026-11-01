#pragma once

#include "hle/hle_common.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gungnir::hle {

// sceAudio output channels (T6f boot discoveries). The device backend is
// supplied by the host through audio::AudioSink; without one, handing a buffer
// to a channel stops the runtime with a named error.
void register_audio(HleRegistrar &hle);
void reset_audio_state();

// Snapshot export/import (T15c). The section is 8 fixed 48-byte channel
// records; the mixer staging buffer is derived and never saved. Import is
// fail-closed and reattaches each Audio wait's drain poll/cancel closures
// from the channel index; a parked waiter with no matching thread fails with
// snapshot_dangling_wait.
[[nodiscard]] bool export_snapshot_audio(std::vector<std::uint8_t> &bytes_out, std::string &error);
bool import_snapshot_audio(std::span<const std::uint8_t> bytes, Kernel &kernel, std::string &error);

} // namespace gungnir::hle

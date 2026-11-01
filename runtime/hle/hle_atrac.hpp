#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceAtrac3plus (T6h): ATRAC3/3+ track IDs and decode. The decoder is the
// vendored FFmpeg n4.4 subset; guest data is copied into host memory and no
// guest pointer reaches the codec.
void register_atrac(HleRegistrar &hle);
void reset_atrac_state();

// Snapshot export/import for ATRAC slots (T15c). Each used slot stores its
// source bytes; import re-opens the decoder from those bytes and restores the
// play position. Decoder overlap state is re-primed from the position, so PCM
// output at the resume point is best-effort while IDs/positions/loops are
// exact. Import is fail-closed.
[[nodiscard]] bool export_snapshot_atrac(std::vector<std::uint8_t> &bytes_out, std::string &error);
bool import_snapshot_atrac(std::span<const std::uint8_t> bytes, std::string &error);

} // namespace gungnir::hle

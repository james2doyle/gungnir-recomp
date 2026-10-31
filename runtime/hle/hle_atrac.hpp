#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceAtrac3plus (T6h): ATRAC3/3+ track IDs and decode. The decoder is the
// vendored FFmpeg n4.4 subset; guest data is copied into host memory and no
// guest pointer reaches the codec.
void register_atrac(HleRegistrar &hle);
void reset_atrac_state();

} // namespace gungnir::hle

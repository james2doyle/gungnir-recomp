#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceMpeg is outside the M4 scope (docs/HLE.md). Every entry point is bound to
// an explicit "no RAPI / not initialized" error and recorded as a named gap so
// the guest can skip movie playback instead of stopping at an unknown NID.
void register_mpeg(HleRegistrar &hle);

} // namespace gungnir::hle

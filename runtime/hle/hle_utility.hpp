#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceUtility dialogs (T6k). Savedata is the first title-executed family.
void register_utility(HleRegistrar &hle);
void reset_utility_state();

} // namespace gungnir::hle

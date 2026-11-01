#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceUtility dialogs. Savedata (SIZES, LIST, LOAD, SAVE) and the headless
// message dialog are the title-executed families.
void register_utility(HleRegistrar &hle);
void reset_utility_state();

} // namespace gungnir::hle

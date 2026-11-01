#pragma once

#include "hle/hle_common.hpp"

#include <span>
#include <string>
#include <vector>

namespace gungnir::hle {

// sceUtility dialogs. Savedata (SIZES, LIST, LOAD, SAVE) and the headless
// message dialog are the title-executed families.
void register_utility(HleRegistrar &hle);
void reset_utility_state();

// Snapshot export/import for the savedata/message-dialog status machines
// (T15c). Only the host-side machines are saved; the parameter blocks live in
// guest memory (saved) and the memory-stick contents are session config and
// are never snapshotted. Import is fail-closed.
[[nodiscard]] bool export_snapshot_utility(std::vector<std::uint8_t> &bytes_out, std::string &error);
bool import_snapshot_utility(std::span<const std::uint8_t> bytes, std::string &error);

} // namespace gungnir::hle

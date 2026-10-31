#pragma once

// Versioned touch-layout persistence (T11b).
//
// The file is JSON from TouchOverlay::to_json(); writes go to a sibling
// temporary file and are renamed into place, so a crash or a full disk leaves
// the previous layout intact. The view supplies the app-private path.

#include "input/touch_overlay.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace gungnir::input {

enum class TouchLayoutStatus : std::uint8_t {
    Ok,
    Missing,
    Older,
    Corrupt,
    WriteFailed,
};

struct TouchLayoutResult {
    TouchLayoutStatus status{TouchLayoutStatus::Ok};
    std::string error;
};

// Loads into `overlay`; Missing/Older/Corrupt leave the overlay untouched.
[[nodiscard]] TouchLayoutResult load_touch_layout(
    const std::filesystem::path &path, TouchOverlay &overlay);

// Writes atomically into `path`. A failure removes the temporary file and
// leaves the previous layout intact.
[[nodiscard]] TouchLayoutResult save_touch_layout(
    const std::filesystem::path &path, const TouchOverlay &overlay);

} // namespace gungnir::input

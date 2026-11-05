#ifndef GUNGNIR_GE_WIDESCREEN_COVERAGE_HPP
#define GUNGNIR_GE_WIDESCREEN_COVERAGE_HPP

#include "ge/state_machine.hpp"

#include <vector>

namespace gungnir::ge {

struct CoverageResult {
    int coverage_percent{};       // 0..100, columns with submitted guest geometry
    int covered_columns{};        // columns with at least one geometry draw
    int backdrop_only_columns{};  // columns with only the stretched backdrop / nothing
    int total_columns{};          // presented width in guest-pixel columns
};

// Bounds-only measure of the widescreen gap. The guest culls 3D against its
// native 480-wide camera, so columns with no submitted geometry show only the
// stretched full-frame 2D backdrop.
//
// Policy (pinned by tests/ge/widescreen_coverage_tests.cpp): a draw is
// "geometry" unless it is a through (screen-space) draw whose bounds cover the
// guest frame -- those are the stretched backdrop / full-screen effects. A
// small through HUD panel is geometry. Through draws are mapped with the same
// wide_screen_x split as presentation; 3D draws are already projected into the
// widened virtual frame, so their bounds are used as-is.
//
// Cheap: one pass over draws and one pass over <= frame_width column flags.
// No render pass, no per-pixel work.
CoverageResult measure_widescreen_coverage(const std::vector<Draw> &draws, float scale,
                                           int guest_width = 480, int guest_height = 272);

}  // namespace gungnir::ge

#endif  // GUNGNIR_GE_WIDESCREEN_COVERAGE_HPP

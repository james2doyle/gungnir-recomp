#include "ge/widescreen_coverage.hpp"

#include "ge/state_machine.hpp"
#include "ge/vertex.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace gungnir::ge {

namespace {

bool is_stretched_backdrop(const Draw &draw, int guest_width, int guest_height) {
    return draw.state.through && covers_guest_frame(draw.vertices, guest_width, guest_height);
}

}  // namespace

CoverageResult measure_widescreen_coverage(const std::vector<Draw> &draws, float scale,
                                           int guest_width, int guest_height) {
    const float widen = scale > 1.0f ? scale : 1.0f;
    const int total = std::max(1, static_cast<int>(std::lround(
        static_cast<float>(guest_width) * widen)));
    std::vector<std::uint8_t> geometry(total, 0u);

    for (const Draw &draw : draws) {
        if (draw.vertices.empty()) continue;
        const bool backdrop = is_stretched_backdrop(draw, guest_width, guest_height);
        if (backdrop) continue;  // fills the margins, but is not submitted geometry
        float min_x = draw.vertices.front().x;
        float max_x = min_x;
        for (const ScreenVertex &vertex : draw.vertices) {
            min_x = std::min(min_x, vertex.x);
            max_x = std::max(max_x, vertex.x);
        }
        if (draw.state.through) {
            min_x = wide_screen_x(min_x, widen, false);
            max_x = wide_screen_x(max_x, widen, false);
        }
        const int first = std::clamp(static_cast<int>(std::floor(min_x)), 0, total);
        const int last = std::clamp(static_cast<int>(std::ceil(max_x)), 0, total);
        for (int column = first; column < last; ++column) geometry[column] = 1u;
    }

    int covered = 0;
    for (std::uint8_t flag : geometry) covered += flag != 0u ? 1 : 0;
    CoverageResult result;
    result.total_columns = total;
    result.covered_columns = covered;
    result.backdrop_only_columns = total - covered;
    result.coverage_percent = total > 0 ? (covered * 100) / total : 0;
    return result;
}

}  // namespace gungnir::ge

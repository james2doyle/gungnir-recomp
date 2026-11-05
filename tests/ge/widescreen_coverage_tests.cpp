// 9826wejc — host contracts for the widescreen coverage measure.
//
// The named gap is measured, not described: in a widened frame, columns that
// only show the stretched full-frame 2D backdrop (or nothing) have no submitted
// guest geometry, because the guest culls against the native 480-wide camera.
// Coverage is the fraction of presented columns with at least one geometry
// draw; backdrop_only_columns counts the rest.
//
// Policy: a draw is "geometry" unless it is a through (screen-space) draw whose
// bounds cover the guest frame -- those are the stretched backdrop/full-frame
// effects. A HUD panel (through, small bounds) is geometry. This is a bounds-
// only approximation: no per-pixel work, no extra render pass.
//
// Red-first: written before runtime/ge/widescreen_coverage.* existed.

#include "ge/widescreen_coverage.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using gungnir::ge::CoverageResult;
using gungnir::ge::Draw;
using gungnir::ge::measure_widescreen_coverage;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

Draw make_draw(float x0, float y0, float x1, float y1, bool through) {
    Draw draw;
    draw.primitive = gungnir::ge::Primitive::Triangles;
    draw.state.through = through;
    draw.vertices.resize(4);
    draw.vertices[0].x = x0; draw.vertices[0].y = y0;
    draw.vertices[1].x = x1; draw.vertices[1].y = y0;
    draw.vertices[2].x = x1; draw.vertices[2].y = y1;
    draw.vertices[3].x = x0; draw.vertices[3].y = y1;
    return draw;
}

void an_empty_frame_is_all_backdrop() {
    const CoverageResult result = measure_widescreen_coverage({}, 1.3001f, 480);
    check(result.total_columns > 0, "there is a presented width to measure");
    check(result.coverage_percent == 0, "an empty frame has no guest geometry");
    check(result.backdrop_only_columns == result.total_columns,
        "every column of an empty frame is backdrop-only");
}

void a_single_full_frame_quad_covers_everything() {
    // A non-through quad spanning the whole widened frame: no backdrop columns.
    const float wide = 480.0f * 1.3001f;
    std::vector<Draw> draws{make_draw(0.0f, 0.0f, wide, 272.0f, false)};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.3001f, 480);
    check(result.coverage_percent == 100, "a full-frame geometry quad covers everything");
    check(result.backdrop_only_columns == 0, "no backdrop-only columns remain");
}

void a_stretched_full_frame_through_draw_is_the_backdrop_not_geometry() {
    // The stretched backdrop is a through draw covering the guest frame; it
    // fills the margins but must not be counted as submitted guest geometry.
    std::vector<Draw> draws{make_draw(0.0f, 0.0f, 480.0f, 272.0f, true)};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.3001f, 480);
    check(result.coverage_percent == 0, "the backdrop alone is not geometry");
    check(result.backdrop_only_columns == result.total_columns,
        "all columns are backdrop-only when only the backdrop was drawn");
}

void a_small_through_hud_panel_is_geometry() {
    // A HUD panel does not cover the guest frame, so it is not the backdrop.
    std::vector<Draw> draws{make_draw(10.0f, 10.0f, 60.0f, 40.0f, true)};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.3001f, 480);
    check(result.coverage_percent > 0 && result.coverage_percent < 100,
        "a small HUD panel covers some columns, not all");
    check(result.backdrop_only_columns > 0,
        "columns outside the HUD panel remain backdrop-only");
}

void partial_geometry_leaves_the_margins_backdrop_only() {
    // 3D geometry submitted only for the native 480-wide centre of a 624-wide
    // presented frame: the outer columns are backdrop-only. This is the gap.
    const float wide = 480.0f * 1.3001f;
    const float centre = (wide - 480.0f) / 2.0f;
    std::vector<Draw> draws{
        make_draw(centre, 0.0f, centre + 480.0f, 272.0f, false),
        make_draw(0.0f, 0.0f, 480.0f, 272.0f, true),  // stretched backdrop
    };
    const CoverageResult result = measure_widescreen_coverage(draws, 1.3001f, 480);
    check(result.coverage_percent > 0 && result.coverage_percent < 100,
        "partial geometry gives partial coverage");
    check(result.backdrop_only_columns == result.total_columns - result.covered_columns,
        "backdrop-only columns are exactly the uncovered ones");
}

void a_native_scale_frame_is_fully_covered_by_a_full_frame_draw() {
    std::vector<Draw> draws{make_draw(0.0f, 0.0f, 480.0f, 272.0f, false)};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.0f, 480);
    check(result.total_columns == 480, "at native scale the presented width is 480 columns");
    check(result.coverage_percent == 100, "a native full-frame draw covers all columns");
    check(result.backdrop_only_columns == 0, "no backdrop columns at native scale");
}

void a_draw_partly_outside_the_frame_is_clipped() {
    std::vector<Draw> draws{make_draw(-100.0f, 0.0f, 1000.0f, 272.0f, false)};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.0f, 480);
    check(result.covered_columns == 480, "draw bounds are clipped to the frame");
    check(result.backdrop_only_columns == 0, "clipping leaves no uncovered columns");
}

void a_draw_with_no_vertices_contributes_nothing() {
    Draw empty;
    empty.state.through = false;
    std::vector<Draw> draws{empty};
    const CoverageResult result = measure_widescreen_coverage(draws, 1.0f, 480);
    check(result.coverage_percent == 0, "a vertex-less draw covers nothing");
    check(result.backdrop_only_columns == 480, "all columns remain backdrop-only");
}

}  // namespace

int main() {
    try {
        an_empty_frame_is_all_backdrop();
        a_single_full_frame_quad_covers_everything();
        a_stretched_full_frame_through_draw_is_the_backdrop_not_geometry();
        a_small_through_hud_panel_is_geometry();
        partial_geometry_leaves_the_margins_backdrop_only();
        a_native_scale_frame_is_fully_covered_by_a_full_frame_draw();
        a_draw_partly_outside_the_frame_is_clipped();
        a_draw_with_no_vertices_contributes_nothing();
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    std::cout << "widescreen_coverage_tests: 8 contracts passed\n";
    return 0;
}

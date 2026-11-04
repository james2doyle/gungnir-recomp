// T17a.5 — renderer interpolation contracts.
//
// In-between vertices are derived from two frames the guest already produced;
// guest RAM is never touched, so the pass cannot change game behaviour. It must
// leave the real frame's untouched cases exactly as produced: unmatched draws,
// 2D/HUD (through) draws and camera cuts.
//
// Red-first: written before interpolate_frame existed.

#include "ge/frame_interpolation.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using gungnir::ge::Draw;
using gungnir::ge::FrameHistory;
using gungnir::ge::InterpolationStats;
using gungnir::ge::interpolate_frame;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void check_near(float actual, float expected, const char *contract) {
    if (std::fabs(actual - expected) > 0.001f) throw std::runtime_error(contract);
}

Draw quad(float x, float y, std::uint64_t texture = 0x1234u) {
    Draw draw;
    draw.primitive = gungnir::ge::Primitive::Triangles;
    draw.state.texture_key = texture;
    draw.state.texture_enabled = true;
    draw.state.draw_format = 3u;
    draw.vertices.resize(4u);
    for (std::size_t i = 0; i < 4u; ++i) {
        draw.vertices[i].x = x + static_cast<float>(i % 2u) * 10.0f;
        draw.vertices[i].y = y + static_cast<float>(i / 2u) * 10.0f;
        draw.vertices[i].z = 5.0f;
        draw.vertices[i].inv_w = 1.0f;
    }
    return draw;
}

void a_two_frame_pan_interpolates_its_midpoint() {
    FrameHistory history;
    history.push({quad(0.0f, 0.0f)});
    std::vector<Draw> frame{quad(10.0f, 0.0f)};
    InterpolationStats stats;
    interpolate_frame(frame, history, 0.5f, &stats);
    check(stats.candidates == 1u && stats.matched == 1u,
        "a matched transform draw is interpolated");
    check_near(frame[0].vertices[0].x, 5.0f, "the midpoint pan is halfway between the frames");
    check_near(frame[0].vertices[0].z, 5.0f, "a stationary axis stays put");
    check_near(frame[0].vertices[0].inv_w, 1.0f, "inv_w is carried through");
}

void the_ends_reproduce_the_real_frames() {
    FrameHistory history;
    history.push({quad(0.0f, 0.0f)});
    std::vector<Draw> frame{quad(12.0f, 4.0f)};
    interpolate_frame(frame, history, 0.0f);
    check_near(frame[0].vertices[0].x, 0.0f, "t = 0 is the previous frame");
    check_near(frame[0].vertices[0].y, 0.0f, "t = 0 is the previous frame vertically");
    std::vector<Draw> at_one{quad(12.0f, 4.0f)};
    interpolate_frame(at_one, history, 1.0f);
    check_near(at_one[0].vertices[0].x, 12.0f, "t = 1 is this frame");
    check_near(at_one[0].vertices[0].y, 4.0f, "t = 1 is this frame vertically");
}

void an_unmatched_draw_passes_through() {
    FrameHistory history;
    history.push({quad(0.0f, 0.0f, 0x1111u)});
    std::vector<Draw> frame{quad(40.0f, 0.0f, 0x2222u)};
    InterpolationStats stats;
    interpolate_frame(frame, history, 0.5f, &stats);
    check(stats.candidates == 1u && stats.matched == 0u && stats.unmatched == 1u,
        "a draw with no partner is counted as unmatched");
    check_near(frame[0].vertices[0].x, 40.0f, "an unmatched draw keeps this frame's vertices");
}

void a_through_draw_is_never_interpolated() {
    FrameHistory history;
    Draw previous = quad(0.0f, 0.0f);
    previous.state.through = true;
    history.push({previous});
    Draw current = quad(30.0f, 0.0f);
    current.state.through = true;
    std::vector<Draw> frame{current};
    InterpolationStats stats;
    interpolate_frame(frame, history, 0.5f, &stats);
    check(stats.candidates == 0u, "a through (2D/HUD) draw is not even a candidate");
    check_near(frame[0].vertices[0].x, 30.0f, "2D/HUD keeps this frame's position");
}

void a_camera_cut_is_rejected() {
    FrameHistory history;
    history.push({quad(0.0f, 0.0f)});
    // 300 px in a 480-wide frame is far past the cut fraction; a camera cut
    // must snap to the new frame rather than drag the old scene across.
    std::vector<Draw> frame{quad(300.0f, 0.0f)};
    InterpolationStats stats;
    interpolate_frame(frame, history, 0.5f, &stats);
    check(stats.cut == 1u && stats.matched == 0u, "a jump past the threshold is a cut");
    check_near(frame[0].vertices[0].x, 300.0f, "a rejected pair is left untouched");
}

void repeated_occurrences_keep_their_order() {
    FrameHistory history;
    history.push({quad(0.0f, 0.0f), quad(100.0f, 0.0f)});
    std::vector<Draw> frame{quad(20.0f, 0.0f), quad(120.0f, 0.0f)};
    InterpolationStats stats;
    interpolate_frame(frame, history, 0.5f, &stats);
    check(stats.matched == 2u, "both occurrences match");
    check_near(frame[0].vertices[0].x, 10.0f, "the first occurrence interpolates with the first");
    check_near(frame[1].vertices[0].x, 110.0f, "the second occurrence interpolates with the second");
}

} // namespace

int main() {
    try {
        a_two_frame_pan_interpolates_its_midpoint();
        the_ends_reproduce_the_real_frames();
        an_unmatched_draw_passes_through();
        a_through_draw_is_never_interpolated();
        a_camera_cut_is_rejected();
        repeated_occurrences_keep_their_order();
        std::cout << "interpolation_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

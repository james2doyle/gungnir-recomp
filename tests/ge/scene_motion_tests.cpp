// Contracts for the static-scene detector. Kept in its own binary so the
// intentional flicker reproduction test in interpolation_tests.cpp (which
// aborts that file's main() before later tests) cannot mask these results.

#include "ge/frame_interpolation.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using gungnir::ge::Draw;
using gungnir::ge::Primitive;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void check_near(float actual, float expected, const char *contract) {
    if (std::fabs(actual - expected) > 0.01f) throw std::runtime_error(contract);
}

Draw quad(float x, float y) {
    Draw draw;
    draw.primitive = Primitive::Triangles;
    draw.state.texture_key = 0x1234u;
    draw.vertices.resize(4u);
    for (std::size_t i = 0u; i < 4u; ++i) {
        draw.vertices[i].x = x + static_cast<float>(i % 2u) * 10.0f;
        draw.vertices[i].y = y + static_cast<float>(i / 2u) * 10.0f;
    }
    return draw;
}

// Two identical frames have zero motion and are static.
void identical_frames_are_static() {
    std::vector<Draw> frame{quad(0.0f, 0.0f), quad(100.0f, 40.0f)};
    check_near(gungnir::ge::scene_motion(frame, frame), 0.0f,
        "two identical frames have zero motion");
    check(gungnir::ge::scene_is_static(frame, frame),
        "two identical frames are a static scene");
}

// A whole-scene shift is measured, in guest pixels, mean per vertex.
void a_moved_scene_is_measured_and_not_static() {
    std::vector<Draw> before{quad(0.0f, 0.0f)};
    std::vector<Draw> after{quad(30.0f, 0.0f)};
    check_near(gungnir::ge::scene_motion(after, before), 30.0f,
        "a quad shifted 30 px has 30 px of mean motion");
    check(!gungnir::ge::scene_is_static(after, before),
        "a moved scene is not static");
}

// Sub-pixel drift (a blinking cursor, camera rounding) stays within the
// static threshold.
void subpixel_drift_is_still_static() {
    std::vector<Draw> before{quad(0.0f, 0.0f)};
    std::vector<Draw> after{quad(0.1f, 0.0f)};
    check(gungnir::ge::scene_is_static(after, before),
        "sub-pixel drift stays within the static threshold");
}

// Draws added/removed between frames have no partner and must not invent
// motion: a culling/growing stream is not a reason to interpolate.
void a_changed_draw_count_does_not_invent_motion() {
    std::vector<Draw> before{quad(0.0f, 0.0f)};
    std::vector<Draw> after{quad(0.0f, 0.0f), quad(50.0f, 50.0f)};
    check(gungnir::ge::scene_is_static(after, before),
        "extra draws without a partner do not count as motion");
}

// An empty frame is static (nothing to interpolate), not a spurious pan.
void empty_frames_are_static() {
    std::vector<Draw> before;
    std::vector<Draw> after;
    check_near(gungnir::ge::scene_motion(after, before), 0.0f,
        "empty frames have zero motion");
    check(gungnir::ge::scene_is_static(after, before),
        "empty frames are static");
}

// frames_identical is the exact "hold the last image" gate: the swapchain image
// is only safe to keep when the new guest frame is the same picture.
void identical_frames_compare_equal() {
    std::vector<Draw> a{quad(0.0f, 0.0f), quad(100.0f, 40.0f)};
    std::vector<Draw> b = a;
    check(gungnir::ge::frames_identical(a, b),
        "a frame copied verbatim is identical");
    check(gungnir::ge::frames_identical(a, a),
        "a frame compared with itself is identical");
}

void a_moved_vertex_makes_frames_different() {
    std::vector<Draw> a{quad(0.0f, 0.0f)};
    std::vector<Draw> b{quad(0.5f, 0.0f)};  // below the static-motion threshold
    check(gungnir::ge::scene_is_static(b, a),
        "the motion heuristic would call this static");
    check(!gungnir::ge::frames_identical(a, b),
        "but exact frame identity still sees the changed vertex");
}

void a_changed_draw_count_makes_frames_different() {
    std::vector<Draw> a{quad(0.0f, 0.0f)};
    std::vector<Draw> b{quad(0.0f, 0.0f), quad(50.0f, 50.0f)};
    check(!gungnir::ge::frames_identical(a, b),
        "a different draw count is not an identical frame");
}

void a_changed_texture_identity_makes_frames_different() {
    std::vector<Draw> a{quad(0.0f, 0.0f)};
    std::vector<Draw> b{quad(0.0f, 0.0f)};
    b[0].state.texture_key = 0xBEEFu;
    check(!gungnir::ge::frames_identical(a, b),
        "draw_key includes texture identity");
}

void changed_indices_make_frames_different() {
    std::vector<Draw> a{quad(0.0f, 0.0f)};
    std::vector<Draw> b{quad(0.0f, 0.0f)};
    b[0].indices.push_back(1u);
    check(!gungnir::ge::frames_identical(a, b),
        "changed indices are not an identical frame");
}

} // namespace

int main() {
    try {
        identical_frames_are_static();
        a_moved_scene_is_measured_and_not_static();
        subpixel_drift_is_still_static();
        a_changed_draw_count_does_not_invent_motion();
        empty_frames_are_static();
        identical_frames_compare_equal();
        a_moved_vertex_makes_frames_different();
        a_changed_draw_count_makes_frames_different();
        a_changed_texture_identity_makes_frames_different();
        changed_indices_make_frames_different();
        std::cout << "scene_motion_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

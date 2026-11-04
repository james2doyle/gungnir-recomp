// T17a.4 — draw identity and previous-frame record contracts.
//
// The interpolator will match draws across frames by what they are. This pins
// the parts that break naively: the alternating display buffer must not change
// the key, a repeated draw must keep its order, and a through-mode (2D/HUD)
// draw must never be a candidate.
//
// Red-first: written before runtime/ge/frame_interpolation.* existed.

#include "ge/frame_interpolation.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

using gungnir::ge::Draw;
using gungnir::ge::FrameHistory;
using gungnir::ge::draw_key;
using gungnir::ge::interpolatable;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

Draw make_draw(std::uint32_t buffer, std::uint64_t texture_key, std::size_t vertices) {
    Draw draw;
    draw.primitive = gungnir::ge::Primitive::Triangles;
    draw.vertices.resize(vertices);
    draw.state.draw_buffer = buffer;
    draw.state.texture_key = texture_key;
    draw.state.draw_format = 3u;
    return draw;
}

void the_same_draw_keeps_its_key() {
    const Draw first = make_draw(0x04000000u, 0x11223344u, 4u);
    const Draw second = make_draw(0x04000000u, 0x11223344u, 4u);
    check(draw_key(first) == draw_key(second), "the same draw keys the same twice");
}

void the_alternating_display_buffer_does_not_change_the_key() {
    const Draw back = make_draw(0x04000000u, 0x11223344u, 4u);
    const Draw front = make_draw(0x04044000u, 0x11223344u, 4u);
    check(draw_key(back) == draw_key(front),
        "the target address is not identity: the game alternates display buffers");
}

void a_changed_texture_or_vertex_count_changes_the_key() {
    const Draw base = make_draw(0x04000000u, 0x11223344u, 4u);
    check(draw_key(base) != draw_key(make_draw(0x04000000u, 0x55667788u, 4u)),
        "a different texture is a different draw");
    check(draw_key(base) != draw_key(make_draw(0x04000000u, 0x11223344u, 6u)),
        "a different vertex count is a different draw");
    Draw through = base;
    through.state.through = true;
    check(draw_key(base) != draw_key(through),
        "a through draw is not the same draw as a transform draw");
}

void through_draws_are_never_interpolated() {
    Draw sprite = make_draw(0x04000000u, 0u, 4u);
    check(interpolatable(sprite), "a transform draw with vertices is a candidate");
    sprite.state.through = true;
    check(!interpolatable(sprite), "a through (2D/HUD) draw is never a candidate");
    Draw degenerate = make_draw(0x04000000u, 0u, 2u);
    check(!interpolatable(degenerate), "a draw without a triangle is never a candidate");
}

void history_matches_in_occurrence_order() {
    FrameHistory history;
    check(history.empty(), "a fresh history holds no frame");
    std::vector<Draw> frame;
    frame.push_back(make_draw(0x04000000u, 0xAAu, 4u));
    frame.push_back(make_draw(0x04000000u, 0xAAu, 4u));
    frame.push_back(make_draw(0x04000000u, 0xBBu, 4u));
    frame[0].vertices[0].x = 10.0f;
    frame[1].vertices[0].x = 20.0f;
    history.push(std::move(frame));
    check(!history.empty(), "the pushed frame is the previous frame");

    const Draw repeated = make_draw(0x04044000u, 0xAAu, 4u);
    const Draw *first = history.match(repeated, 0u);
    const Draw *second = history.match(repeated, 1u);
    check(first != nullptr && first->vertices[0].x == 10.0f,
        "the first occurrence matches the first previous draw");
    check(second != nullptr && second->vertices[0].x == 20.0f,
        "the second occurrence matches the second previous draw, not the first");
    check(history.match(repeated, 2u) == nullptr, "no third partner exists");
    check(history.match(make_draw(0x04000000u, 0xCCu, 4u), 0u) == nullptr,
        "an unseen draw has no partner");
    check(history.matches() == 2u, "the history counts the lookups that matched");

    history.clear();
    check(history.empty() && history.match(repeated, 0u) == nullptr,
        "a cleared history has no partners");
}

} // namespace

int main() {
    try {
        the_same_draw_keeps_its_key();
        the_alternating_display_buffer_does_not_change_the_key();
        a_changed_texture_or_vertex_count_changes_the_key();
        through_draws_are_never_interpolated();
        history_matches_in_occurrence_order();
        std::cout << "draw_identity_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

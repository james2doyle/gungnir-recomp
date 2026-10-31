// T11b touch-overlay contracts: default layout, viewport/safe-area mapping,
// multi-pointer hit-testing, floating stick, d-pad directions, edit clamping
// and the versioned JSON layout.

#include "input/touch_layout_store.hpp"
#include "input/touch_overlay.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <fstream>

namespace {

using gungnir::input::Analog;
using gungnir::input::TouchControl;
using gungnir::input::TouchInsets;
using gungnir::input::TouchOverlay;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

bool near(float a, float b, float epsilon = 0.001f) {
    return std::fabs(a - b) <= epsilon;
}

void press(TouchOverlay &overlay, int id, float x, float y) {
    overlay.touch_down(id, x, y);
}

void move_to(TouchOverlay &overlay, int id, float x, float y) {
    overlay.touch_move(id, x, y);
}

void release(TouchOverlay &overlay, int id) {
    overlay.touch_up(id);
}

float snapshot_x(const TouchOverlay &overlay, TouchControl control) {
    return overlay.snapshot()[static_cast<std::size_t>(control)].cx;
}

float snapshot_y(const TouchOverlay &overlay, TouchControl control) {
    return overlay.snapshot()[static_cast<std::size_t>(control)].cy;
}

float snapshot_radius(const TouchOverlay &overlay, TouchControl control) {
    return overlay.snapshot()[static_cast<std::size_t>(control)].radius;
}

bool snapshot_pressed(const TouchOverlay &overlay, TouchControl control) {
    return overlay.snapshot()[static_cast<std::size_t>(control)].pressed;
}

void defaults_and_viewport() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    check(overlay.placements().size() == gungnir::input::kTouchControls,
          "every control has a placement");
    check(overlay.placement(TouchControl::Cross).visible, "face buttons start visible");
    check(overlay.placement(TouchControl::L).visible, "shoulder buttons start visible");
    check(overlay.snapshot()[static_cast<std::size_t>(TouchControl::Cross)].radius > 0.0f,
          "defaults carry a drawable radius");

    const auto cross = overlay.placement(TouchControl::Cross);
    check(near(cross.cx, 0.82f) && near(cross.cy, 0.74f), "Cross defaults to the right diamond");
    check(near(overlay.placement(TouchControl::Circle).cx, 0.90f), "Circle defaults right of Cross");
    check(near(overlay.placement(TouchControl::Square).cx, 0.74f), "Square defaults left of Cross");
    check(near(overlay.placement(TouchControl::Triangle).cy, 0.50f), "Triangle defaults above Cross");

    overlay.set_viewport(1280, 720, TouchInsets{100.0f, 20.0f, 50.0f, 30.0f});
    check(near(snapshot_x(overlay, TouchControl::Cross), 100.0f + 0.82f * 1130.0f, 0.01f),
          "safe-area left inset shifts the control");
    check(near(snapshot_y(overlay, TouchControl::Cross), 20.0f + 0.74f * 670.0f, 0.01f),
          "safe-area top inset shifts the control");
    check(near(snapshot_radius(overlay, TouchControl::Cross), 0.05f * 670.0f, 0.01f),
          "radius scales with the safe content height");
}

void single_touch_and_slide() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    const auto cross = overlay.placement(TouchControl::Cross);
    const auto circle = overlay.placement(TouchControl::Circle);
    const float cx = 0.0f; // unused; keep the test explicit about pixel coordinates
    (void)cx;

    press(overlay, 1, 0.82f * 1280.0f, 0.74f * 720.0f);
    check(overlay.buttons() == gungnir::input::kCrossBit, "Cross touch presses Cross");
    check(snapshot_pressed(overlay, TouchControl::Cross), "Cross draws pressed");

    move_to(overlay, 1, 0.90f * 1280.0f, 0.62f * 720.0f);
    check(overlay.buttons() == gungnir::input::kCircleBit, "sliding to Circle releases Cross");
    check(!snapshot_pressed(overlay, TouchControl::Cross), "Cross stops drawing pressed");
    check(snapshot_pressed(overlay, TouchControl::Circle), "Circle draws pressed");

    move_to(overlay, 1, 0.50f * 1280.0f, 0.50f * 720.0f);
    check(overlay.buttons() == 0u, "sliding off every control releases it");
    check(!snapshot_pressed(overlay, TouchControl::Circle), "Circle stops drawing pressed");

    press(overlay, 2, 0.82f * 1280.0f, 0.74f * 720.0f);
    release(overlay, 2);
    check(overlay.buttons() == 0u, "lifting the finger releases the button");
    (void)cross;
    (void)circle;
}

void multi_touch() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    press(overlay, 3, 0.82f * 1280.0f, 0.74f * 720.0f);
    press(overlay, 7, 0.06f * 1280.0f, 0.10f * 720.0f);
    check(overlay.buttons() == (gungnir::input::kCrossBit | gungnir::input::kLBit),
          "two fingers hold two buttons");
    release(overlay, 3);
    check(overlay.buttons() == gungnir::input::kLBit, "releasing one finger keeps the other");
    release(overlay, 7);
    check(overlay.buttons() == 0u, "releasing the last finger clears the pad");
}

void dpad_directions() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    const auto dpad = overlay.placement(TouchControl::Dpad);
    const float cx = dpad.cx * 1280.0f;
    const float cy = dpad.cy * 720.0f;
    const float reach = dpad.radius * 720.0f;

    press(overlay, 1, cx, cy - reach * 0.8f);
    check(overlay.buttons() == gungnir::input::kUpBit, "d-pad up");
    move_to(overlay, 1, cx + reach * 0.8f, cy);
    check(overlay.buttons() == gungnir::input::kRightBit, "d-pad right");
    move_to(overlay, 1, cx + reach * 0.7f, cy - reach * 0.7f);
    check(overlay.buttons() == (gungnir::input::kUpBit | gungnir::input::kRightBit),
          "d-pad diagonal presses both directions");
    move_to(overlay, 1, cx, cy);
    check(overlay.buttons() == 0u, "d-pad centre is neutral");
    release(overlay, 1);
}

void floating_stick() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    press(overlay, 1, 200.0f, 400.0f);
    check(overlay.analog() == Analog{}, "a fresh stick touch is centred");
    check(snapshot_pressed(overlay, TouchControl::Stick), "the stick marker draws active");

    move_to(overlay, 1, 260.0f, 400.0f);
    const float reach = overlay.placement(TouchControl::Stick).radius * 720.0f;
    check(near(overlay.analog().x, 60.0f / reach, 0.002f), "stick x follows the thumb");
    check(near(overlay.analog().y, 0.0f, 0.002f), "stick y stays centred");

    move_to(overlay, 1, 200.0f + reach * 3.0f, 400.0f);
    check(near(overlay.analog().x, 1.0f, 0.002f), "stick clamps beyond its reach");
    const auto active = overlay.active_stick();
    check(near(active.origin_x, 200.0f + reach * 2.0f, 0.01f), "the floating origin follows the thumb");

    release(overlay, 1);
    check(overlay.analog() == Analog{}, "lifting the stick recentres it");
    check(!snapshot_pressed(overlay, TouchControl::Stick), "the stick marker stops drawing active");
}

void cancel_releases_all() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    press(overlay, 1, 0.82f * 1280.0f, 0.74f * 720.0f);
    press(overlay, 2, 200.0f, 400.0f);
    move_to(overlay, 2, 260.0f, 400.0f);
    overlay.cancel();
    check(overlay.buttons() == 0u, "cancel releases every button");
    check(overlay.analog() == Analog{}, "cancel recentres the stick");
}

void edit_move_and_resize() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    const auto original = overlay.placement(TouchControl::Cross);
    overlay.set_editing(true);
    check(overlay.editing(), "edit mode is reported");

    overlay.edit_down(1, 0.82f * 1280.0f, 0.74f * 720.0f);
    overlay.edit_move(1, 1200.0f, 700.0f);
    overlay.edit_up(1);
    const auto moved = overlay.placement(TouchControl::Cross);
    check(moved.cx > original.cx && moved.cy > original.cy, "dragging moves the control");
    const float radius_px = moved.radius * 720.0f;
    check(moved.cx <= 1.0f - radius_px / 1280.0f + 0.0001f, "dragging clamps to the right edge");
    check(moved.cy <= 1.0f - radius_px / 720.0f + 0.0001f, "dragging clamps to the bottom edge");

    const float handle_x = moved.cx * 1280.0f + moved.radius * 0.5f * 720.0f;
    const float handle_y = moved.cy * 720.0f + moved.radius * 0.5f * 720.0f;
    overlay.edit_down(2, handle_x, handle_y);
    overlay.edit_move(2, handle_x + 200.0f, handle_y + 200.0f);
    overlay.edit_up(2);
    check(overlay.placement(TouchControl::Cross).radius > moved.radius, "the handle resizes the control");

    overlay.set_editing(false);
    check(!overlay.editing(), "leaving edit mode is reported");

    overlay.set_editing(true);
    const auto current = overlay.placement(TouchControl::Cross);
    check(overlay.edit_down(3, current.cx * 1280.0f, current.cy * 720.0f),
          "a new edit press selects a control");
    overlay.touch_up(3);
    check(!overlay.edit_move(3, 900.0f, 500.0f), "lifting the edit pointer ends the drag");
    overlay.set_editing(false);
    overlay.reset_layout();
    const auto reset = overlay.placement(TouchControl::Cross);
    check(near(reset.cx, original.cx) && near(reset.cy, original.cy) &&
              near(reset.radius, original.radius),
          "reset restores the default layout");
}

void json_round_trip() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    overlay.set_editing(true);
    overlay.edit_down(1, 0.82f * 1280.0f, 0.74f * 720.0f);
    overlay.edit_move(1, 900.0f, 500.0f);
    overlay.edit_up(1);
    overlay.set_editing(false);
    overlay.resize(TouchControl::L, 0.01f);

    const std::string json = overlay.to_json();
    check(json.find("\"version\"") != std::string::npos, "the layout carries a version");
    check(json.find("\"controls\"") != std::string::npos, "the layout carries a control list");

    TouchOverlay loaded;
    check(loaded.load_json(json) == TouchOverlay::LoadStatus::Ok, "the layout loads");
    check(loaded.to_json() == json, "reloading reproduces the same JSON");
    check(near(loaded.placement(TouchControl::Cross).cx, overlay.placement(TouchControl::Cross).cx),
          "reloaded geometry matches");
    check(near(loaded.placement(TouchControl::L).radius, overlay.placement(TouchControl::L).radius),
          "reloaded radius matches");
}

void store_round_trip() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "gungnir-touch-layout-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / "layout.json";

    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    overlay.resize(TouchControl::Cross, 0.01f);
    const std::string expected = overlay.to_json();
    auto saved = gungnir::input::save_touch_layout(path, overlay);
    check(saved.status == gungnir::input::TouchLayoutStatus::Ok, "store writes the layout");
    check(std::filesystem::is_regular_file(path), "store leaves the layout file");
    check(!std::filesystem::exists(directory / "layout.json.tmp"), "store removes the temporary file");

    TouchOverlay loaded;
    auto result = gungnir::input::load_touch_layout(path, loaded);
    check(result.status == gungnir::input::TouchLayoutStatus::Ok, "store reads the layout");
    check(loaded.to_json() == expected, "store round-trips the layout");

    TouchOverlay missing;
    result = gungnir::input::load_touch_layout(directory / "absent.json", missing);
    check(result.status == gungnir::input::TouchLayoutStatus::Missing, "missing layout is named");
    check(near(missing.placement(TouchControl::Cross).radius, 0.05f), "missing keeps defaults");

    {
        std::ofstream corrupt(path, std::ios::binary | std::ios::trunc);
        corrupt << "{not json";
    }
    TouchOverlay corrupt;
    result = gungnir::input::load_touch_layout(path, corrupt);
    check(result.status == gungnir::input::TouchLayoutStatus::Corrupt, "corrupt layout is named");
    check(near(corrupt.placement(TouchControl::Cross).radius, 0.05f), "corrupt keeps defaults");

    {
        std::ofstream older(path, std::ios::binary | std::ios::trunc);
        older << "{\"version\":0,\"controls\":[]}";
    }
    TouchOverlay older;
    result = gungnir::input::load_touch_layout(path, older);
    check(result.status == gungnir::input::TouchLayoutStatus::Older, "older layout is named");
    check(near(older.placement(TouchControl::Cross).radius, 0.05f), "older keeps defaults");

    auto failed = gungnir::input::save_touch_layout(directory / "missing" / "layout.json", overlay);
    check(failed.status == gungnir::input::TouchLayoutStatus::WriteFailed, "a bad path fails loudly");
    std::filesystem::remove_all(directory, ignored);
}

void json_fallbacks() {
    TouchOverlay overlay;
    overlay.set_viewport(1280, 720, TouchInsets{});
    const auto original = overlay.placement(TouchControl::Cross);

    check(overlay.load_json("{\"version\":0,\"controls\":[]}") == TouchOverlay::LoadStatus::Older,
          "an older schema is reported");
    check(near(overlay.placement(TouchControl::Cross).cx, original.cx), "older schema keeps defaults");

    check(overlay.load_json("{not json") == TouchOverlay::LoadStatus::Corrupt,
          "malformed JSON is reported");
    check(near(overlay.placement(TouchControl::Cross).cx, original.cx), "corrupt JSON keeps defaults");

    check(overlay.load_json("{\"version\":1,\"controls\":[]}") == TouchOverlay::LoadStatus::Corrupt,
          "a missing control list is corrupt");
    check(near(overlay.placement(TouchControl::Cross).cx, original.cx), "corrupt shapes keep defaults");
}

} // namespace

int main() {
    try {
        defaults_and_viewport();
        single_touch_and_slide();
        multi_touch();
        dpad_directions();
        floating_stick();
        cancel_releases_all();
        edit_move_and_resize();
        json_round_trip();
        json_fallbacks();
        store_round_trip();
    } catch (const std::exception &error) {
        std::cerr << "touch_overlay_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "touch_overlay_tests: OK\n";
    return 0;
}

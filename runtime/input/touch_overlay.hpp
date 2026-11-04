#pragma once

// Android touch overlay model (T11b).
//
// The overlay is drawn by a native Android View above the SDL surface; this
// class owns the layout, pointer roles, hit-testing, the floating stick, the
// d-pad directions, edit-mode drag/resize and the versioned JSON layout. It is
// pure host code so the same contracts run without Android.
//
// Coordinates in placements are normalized to the safe content box:
// x/y in [0, 1], radius relative to min(content width, height). Touch input
// and the draw snapshot use window pixels.

#include "input/psp_buttons.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gungnir::input {

enum class TouchControl : std::uint8_t {
    Stick = 0,
    Dpad,
    Triangle,
    Circle,
    Cross,
    Square,
    L,
    R,
    Start,
    Select,
    // Host control, not a PSP button: the touch overlay latches fast-forward
    // and never publishes it to sceCtrl.
    FastForward,
    Count,
};

inline constexpr std::size_t kTouchControls = static_cast<std::size_t>(TouchControl::Count);

struct TouchInsets {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

struct TouchPlacement {
    TouchControl control{};
    float cx{};
    float cy{};
    float radius{};
    bool visible{true};

    friend bool operator==(const TouchPlacement &, const TouchPlacement &) = default;
};

struct TouchDrawControl {
    TouchControl control{};
    float cx{};
    float cy{};
    float radius{};
    bool visible{};
    bool pressed{};
};

struct ActiveStick {
    bool active{};
    int pointer{-1};
    float origin_x{};
    float origin_y{};
    float thumb_x{};
    float thumb_y{};
    float reach{};
};

class TouchOverlay {
public:
    TouchOverlay();

    // Window size and safe-area insets in pixels. Placements stay normalized,
    // so resizing or rotating the window does not move them in content space.
    void set_viewport(int width, int height, TouchInsets insets);
    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

    [[nodiscard]] const std::array<TouchPlacement, kTouchControls> &placements() const noexcept {
        return placements_;
    }
    [[nodiscard]] const TouchPlacement &placement(TouchControl control) const noexcept;
    [[nodiscard]] TouchPlacement &placement(TouchControl control) noexcept;

    // Game input, window pixels. The stick is floating: a finger landing in
    // the left half (and not on a placed control) becomes the stick. JNI
    // publishes buttons()/analog() into InputState after every event.
    [[nodiscard]] TouchControl touch_down(int pointer, float x, float y);
    void touch_move(int pointer, float x, float y);
    void touch_up(int pointer);
    void cancel();
    void release_all();

    [[nodiscard]] ButtonMask buttons() const noexcept;
    [[nodiscard]] Analog analog() const noexcept;
    [[nodiscard]] ActiveStick active_stick() const noexcept;

    // Drawing snapshot in window pixels. `pressed` is true for a held face
    // button, L/R/Start/Select, an active stick, or any d-pad direction.
    [[nodiscard]] std::array<TouchDrawControl, kTouchControls> snapshot() const noexcept;
    [[nodiscard]] ButtonMask dpad_bits() const noexcept;

    // Edit mode. `edit_down` over a control selects it; a touch near its rim
    // resizes, a touch nearer the centre drags. Both clamp to the safe box.
    void set_editing(bool editing) noexcept { editing_ = editing; }
    [[nodiscard]] bool editing() const noexcept { return editing_; }
    bool edit_down(int pointer, float x, float y);
    bool edit_move(int pointer, float x, float y);
    void edit_up(int pointer);
    [[nodiscard]] TouchControl selected() const noexcept { return edit_control_; }

    void move(TouchControl control, float dx, float dy);
    void resize(TouchControl control, float delta_radius);
    void reset_layout();

    // Versioned JSON: normalized placements only, no viewport or insets.
    [[nodiscard]] std::string to_json() const;
    enum class LoadStatus : std::uint8_t { Ok, Older, Corrupt };
    LoadStatus load_json(std::string_view text);

    // Pushes the current touch mask/analog into the shared InputState.
    void publish() const;

private:
    enum class Role : std::uint8_t { None, Control, Dpad, Stick, EditMove, EditResize };
    struct Pointer {
        int id{-1};
        bool used{};
        Role role{Role::None};
        TouchControl control{TouchControl::Count};
        ButtonMask dpad{};
        float last_x{};
        float last_y{};
    };

    [[nodiscard]] Pointer *find(int pointer) noexcept;
    [[nodiscard]] const Pointer *find(int pointer) const noexcept;
    [[nodiscard]] TouchControl control_at(float x, float y, bool include_stick) const noexcept;
    [[nodiscard]] ButtonMask dpad_at(TouchControl control, float x, float y) const noexcept;
    [[nodiscard]] bool control_bit(TouchControl control, ButtonMask &bit) const noexcept;
    [[nodiscard]] bool point_in(const TouchPlacement &placement, float x, float y, float slack) const noexcept;
    void clamp_placement(TouchPlacement &placement) const noexcept;
    void clamp_all() noexcept;
    [[nodiscard]] float content_width() const noexcept;
    [[nodiscard]] float content_height() const noexcept;
    [[nodiscard]] float min_dimension() const noexcept;
    void update_stick_origin(Pointer &pointer, float x, float y) noexcept;

    std::array<TouchPlacement, kTouchControls> placements_{};
    std::array<Pointer, 10> pointers_{};
    int width_{};
    int height_{};
    TouchInsets insets_{};
    bool editing_{};
    TouchControl edit_control_{TouchControl::Count};
    int edit_pointer_{-1};
    Role edit_role_{Role::None};
    float edit_start_cx_{};
    float edit_start_cy_{};
    float edit_start_radius_{};
    float edit_start_x_{};
    float edit_start_y_{};
    float edit_start_distance_{};
    float stick_origin_x_{};
    float stick_origin_y_{};
    float stick_x_{};
    float stick_y_{};
    int stick_pointer_{-1};
};

// Process-wide overlay shared by the JNI bridge and the Android View.
[[nodiscard]] TouchOverlay &touch_overlay() noexcept;
void reset_touch_overlay();

} // namespace gungnir::input

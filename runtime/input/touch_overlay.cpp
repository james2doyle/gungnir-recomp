#include "input/touch_overlay.hpp"

#include "input/host_controls.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace gungnir::input {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinRadius = 0.025f;
constexpr float kMaxRadius = 0.22f;
constexpr float kStickSplit = 0.5f;
// Version 2 adds the fast-forward button. A version 1 file carries every
// control it knew about except fast-forward, which takes its default.
constexpr int kLayoutVersion = 2;
constexpr std::size_t kLayoutV1Controls = 10u;

const char *control_name(TouchControl control) {
    switch (control) {
    case TouchControl::Stick:
        return "stick";
    case TouchControl::Dpad:
        return "dpad";
    case TouchControl::Triangle:
        return "triangle";
    case TouchControl::Circle:
        return "circle";
    case TouchControl::Cross:
        return "cross";
    case TouchControl::Square:
        return "square";
    case TouchControl::L:
        return "l";
    case TouchControl::R:
        return "r";
    case TouchControl::Start:
        return "start";
    case TouchControl::Select:
        return "select";
    case TouchControl::FastForward:
        return "fast_forward";
    case TouchControl::Count:
        break;
    }
    return "";
}

bool control_from_name(std::string_view name, TouchControl &control) {
    for (std::size_t i = 0; i < kTouchControls; ++i) {
        const auto candidate = static_cast<TouchControl>(i);
        if (name == control_name(candidate)) {
            control = candidate;
            return true;
        }
    }
    return false;
}

std::array<TouchPlacement, kTouchControls> default_placements() {
    return {{
        {TouchControl::Stick, 0.17f, 0.52f, 0.110f, true},
        {TouchControl::Dpad, 0.10f, 0.78f, 0.085f, true},
        {TouchControl::Triangle, 0.82f, 0.50f, 0.050f, true},
        {TouchControl::Circle, 0.90f, 0.62f, 0.050f, true},
        {TouchControl::Cross, 0.82f, 0.74f, 0.050f, true},
        {TouchControl::Square, 0.74f, 0.62f, 0.050f, true},
        {TouchControl::L, 0.06f, 0.10f, 0.050f, true},
        {TouchControl::R, 0.94f, 0.10f, 0.050f, true},
        {TouchControl::Start, 0.52f, 0.08f, 0.038f, true},
        {TouchControl::Select, 0.48f, 0.08f, 0.038f, true},
        {TouchControl::FastForward, 0.43f, 0.08f, 0.038f, true},
    }};
}

} // namespace

TouchOverlay::TouchOverlay() {
    reset_layout();
}

void TouchOverlay::set_viewport(int width, int height, TouchInsets insets) {
    width_ = std::max(width, 0);
    height_ = std::max(height, 0);
    insets_ = insets;
    clamp_all();
}

float TouchOverlay::content_width() const noexcept {
    return std::max(0.0f, static_cast<float>(width_) - insets_.left - insets_.right);
}

float TouchOverlay::content_height() const noexcept {
    return std::max(0.0f, static_cast<float>(height_) - insets_.top - insets_.bottom);
}

float TouchOverlay::min_dimension() const noexcept {
    return std::min(content_width(), content_height());
}

const TouchPlacement &TouchOverlay::placement(TouchControl control) const noexcept {
    return placements_[static_cast<std::size_t>(control)];
}

TouchPlacement &TouchOverlay::placement(TouchControl control) noexcept {
    return placements_[static_cast<std::size_t>(control)];
}

bool TouchOverlay::point_in(const TouchPlacement &placement, float x, float y, float slack) const noexcept {
    if (!placement.visible || min_dimension() <= 0.0f) return false;
    const float radius = placement.radius * min_dimension();
    const float cx = insets_.left + placement.cx * content_width();
    const float cy = insets_.top + placement.cy * content_height();
    const float dx = x - cx;
    const float dy = y - cy;
    const float reach = radius + slack;
    return dx * dx + dy * dy <= reach * reach;
}

TouchControl TouchOverlay::control_at(float x, float y, bool include_stick) const noexcept {
    // Face and shoulder controls win over the d-pad when they overlap; the
    // floating stick is never a game hit target, only an edit target.
    constexpr std::array<TouchControl, 10> order{
        TouchControl::Triangle, TouchControl::Circle, TouchControl::Cross, TouchControl::Square,
        TouchControl::L, TouchControl::R, TouchControl::Start, TouchControl::Select,
        TouchControl::FastForward, TouchControl::Dpad};
    for (const TouchControl control : order) {
        if (point_in(placement(control), x, y, 0.0f)) return control;
    }
    if (include_stick && point_in(placement(TouchControl::Stick), x, y, 0.0f))
        return TouchControl::Stick;
    return TouchControl::Count;
}

ButtonMask TouchOverlay::dpad_at(TouchControl control, float x, float y) const noexcept {
    const TouchPlacement &placement = this->placement(control);
    const float reach = placement.radius * min_dimension();
    if (reach <= 0.0f) return 0u;
    const float cx = insets_.left + placement.cx * content_width();
    const float cy = insets_.top + placement.cy * content_height();
    const float dx = x - cx;
    const float dy = y - cy;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < reach * 0.2f) return 0u;
    int sector = static_cast<int>(std::lround(std::atan2(dy, dx) / (kPi / 4.0f)));
    sector = (sector % 8 + 8) % 8;
    switch (sector) {
    case 0:
        return kRightBit;
    case 1:
        return kRightBit | kDownBit;
    case 2:
        return kDownBit;
    case 3:
        return kDownBit | kLeftBit;
    case 4:
        return kLeftBit;
    case 5:
        return kLeftBit | kUpBit;
    case 6:
        return kUpBit;
    case 7:
        return kUpBit | kRightBit;
    default:
        return 0u;
    }
}

bool TouchOverlay::control_bit(TouchControl control, ButtonMask &bit) const noexcept {
    switch (control) {
    case TouchControl::Cross:
        bit = kCrossBit;
        return true;
    case TouchControl::Circle:
        bit = kCircleBit;
        return true;
    case TouchControl::Square:
        bit = kSquareBit;
        return true;
    case TouchControl::Triangle:
        bit = kTriangleBit;
        return true;
    case TouchControl::L:
        bit = kLBit;
        return true;
    case TouchControl::R:
        bit = kRBit;
        return true;
    case TouchControl::Start:
        bit = kStartBit;
        return true;
    case TouchControl::Select:
        bit = kSelectBit;
        return true;
    default:
        return false;
    }
}

TouchOverlay::Pointer *TouchOverlay::find(int pointer) noexcept {
    for (Pointer &entry : pointers_) {
        if (entry.used && entry.id == pointer) return &entry;
    }
    return nullptr;
}

const TouchOverlay::Pointer *TouchOverlay::find(int pointer) const noexcept {
    for (const Pointer &entry : pointers_) {
        if (entry.used && entry.id == pointer) return &entry;
    }
    return nullptr;
}

void TouchOverlay::update_stick_origin(Pointer &pointer, float x, float y) noexcept {
    (void)pointer;
    const float reach = placement(TouchControl::Stick).radius * min_dimension();
    stick_x_ = x;
    stick_y_ = y;
    if (reach <= 0.0f) {
        stick_origin_x_ = x;
        stick_origin_y_ = y;
        return;
    }
    float dx = stick_x_ - stick_origin_x_;
    float dy = stick_y_ - stick_origin_y_;
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance > reach) {
        stick_origin_x_ = stick_x_ - dx / distance * reach;
        stick_origin_y_ = stick_y_ - dy / distance * reach;
        dx = stick_x_ - stick_origin_x_;
        dy = stick_y_ - stick_origin_y_;
    }
}

TouchControl TouchOverlay::touch_down(int pointer, float x, float y) {
    if (editing_) {
        edit_down(pointer, x, y);
        return TouchControl::Count;
    }
    if (Pointer *existing = find(pointer)) {
        (void)existing;
        touch_up(pointer);
    }
    Pointer *entry = nullptr;
    for (Pointer &candidate : pointers_) {
        if (!candidate.used) {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr) return TouchControl::Count;
    *entry = Pointer{};
    entry->id = pointer;
    entry->used = true;
    entry->last_x = x;
    entry->last_y = y;

    const TouchControl hit = control_at(x, y, false);
    if (hit == TouchControl::Dpad) {
        entry->role = Role::Dpad;
        entry->control = TouchControl::Dpad;
        entry->dpad = dpad_at(hit, x, y);
        return TouchControl::Dpad;
    }
    if (hit != TouchControl::Count) {
        // Fast-forward is host execution policy, not a guest button: a tap
        // latches it for the session and the release changes nothing.
        if (hit == TouchControl::FastForward) host_controls().toggle(HostControl::FastForward);
        entry->role = Role::Control;
        entry->control = hit;
        return hit;
    }
    if (x < insets_.left + content_width() * kStickSplit && stick_pointer_ < 0) {
        entry->role = Role::Stick;
        stick_pointer_ = pointer;
        stick_origin_x_ = x;
        stick_origin_y_ = y;
        stick_x_ = x;
        stick_y_ = y;
        return TouchControl::Stick;
    }
    return TouchControl::Count;
}

void TouchOverlay::touch_move(int pointer, float x, float y) {
    if (editing_) {
        edit_move(pointer, x, y);
        return;
    }
    Pointer *entry = find(pointer);
    if (entry == nullptr) return;
    entry->last_x = x;
    entry->last_y = y;
    if (entry->role == Role::Stick) {
        update_stick_origin(*entry, x, y);
        return;
    }
    const TouchControl hit = control_at(x, y, false);
    if (hit == TouchControl::Dpad) {
        entry->role = Role::Dpad;
        entry->control = TouchControl::Dpad;
        entry->dpad = dpad_at(hit, x, y);
    } else if (hit != TouchControl::Count) {
        entry->role = Role::Control;
        entry->control = hit;
        entry->dpad = 0u;
    } else {
        entry->role = Role::None;
        entry->control = TouchControl::Count;
        entry->dpad = 0u;
    }
}

void TouchOverlay::touch_up(int pointer) {
    if (editing_) {
        edit_up(pointer);
        return;
    }
    Pointer *entry = find(pointer);
    if (entry == nullptr) return;
    if (stick_pointer_ == pointer) {
        stick_pointer_ = -1;
        stick_origin_x_ = 0.0f;
        stick_origin_y_ = 0.0f;
        stick_x_ = 0.0f;
        stick_y_ = 0.0f;
    }
    entry->used = false;
    entry->role = Role::None;
    entry->control = TouchControl::Count;
    entry->dpad = 0u;
}

void TouchOverlay::cancel() {
    release_all();
}

void TouchOverlay::release_all() {
    for (Pointer &entry : pointers_) entry = Pointer{};
    stick_pointer_ = -1;
    stick_origin_x_ = 0.0f;
    stick_origin_y_ = 0.0f;
    stick_x_ = 0.0f;
    stick_y_ = 0.0f;
    if (editing_) {
        edit_pointer_ = -1;
        edit_role_ = Role::None;
        edit_control_ = TouchControl::Count;
    }
}

ButtonMask TouchOverlay::dpad_bits() const noexcept {
    ButtonMask bits = 0u;
    for (const Pointer &entry : pointers_) {
        if (entry.used && entry.role == Role::Dpad) bits |= entry.dpad;
    }
    return bits;
}

ButtonMask TouchOverlay::buttons() const noexcept {
    ButtonMask bits = dpad_bits();
    for (const Pointer &entry : pointers_) {
        if (!entry.used || entry.role == Role::Dpad || entry.role == Role::Stick) continue;
        ButtonMask bit = 0u;
        if (control_bit(entry.control, bit)) bits |= bit;
    }
    return bits;
}

Analog TouchOverlay::analog() const noexcept {
    if (stick_pointer_ < 0) return {};
    const float reach = placement(TouchControl::Stick).radius * min_dimension();
    if (reach <= 0.0f) return {};
    const float dx = (stick_x_ - stick_origin_x_) / reach;
    const float dy = (stick_y_ - stick_origin_y_) / reach;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length > 1.0f) return Analog{dx / length, dy / length};
    return Analog{dx, dy};
}

ActiveStick TouchOverlay::active_stick() const noexcept {
    ActiveStick stick;
    if (stick_pointer_ < 0) return stick;
    stick.active = true;
    stick.pointer = stick_pointer_;
    stick.origin_x = stick_origin_x_;
    stick.origin_y = stick_origin_y_;
    stick.thumb_x = stick_x_;
    stick.thumb_y = stick_y_;
    stick.reach = placement(TouchControl::Stick).radius * min_dimension();
    return stick;
}

std::array<TouchDrawControl, kTouchControls> TouchOverlay::snapshot() const noexcept {
    std::array<TouchDrawControl, kTouchControls> result{};
    const ButtonMask dpad = dpad_bits();
    for (std::size_t i = 0; i < kTouchControls; ++i) {
        const TouchPlacement &placement = placements_[i];
        TouchDrawControl &draw = result[i];
        draw.control = placement.control;
        draw.cx = insets_.left + placement.cx * content_width();
        draw.cy = insets_.top + placement.cy * content_height();
        draw.radius = placement.radius * min_dimension();
        draw.visible = placement.visible;
        if (placement.control == TouchControl::Stick) {
            draw.pressed = stick_pointer_ >= 0;
        } else if (placement.control == TouchControl::Dpad) {
            draw.pressed = dpad != 0u;
        } else if (placement.control == TouchControl::FastForward) {
            // Any source lights the button, including a held gamepad trigger.
            draw.pressed = host_controls().active(HostControl::FastForward);
        } else {
            ButtonMask bit = 0u;
            draw.pressed = control_bit(placement.control, bit) && (buttons() & bit) != 0u;
        }
    }
    return result;
}

bool TouchOverlay::edit_down(int pointer, float x, float y) {
    if (!editing_) return false;
    const TouchControl hit = control_at(x, y, true);
    if (hit == TouchControl::Count) return false;
    edit_control_ = hit;
    edit_pointer_ = pointer;
    const TouchPlacement &placement = this->placement(hit);
    edit_start_cx_ = placement.cx;
    edit_start_cy_ = placement.cy;
    edit_start_radius_ = placement.radius;
    edit_start_x_ = x;
    edit_start_y_ = y;
    const float radius = placement.radius * min_dimension();
    const float cx = insets_.left + placement.cx * content_width();
    const float cy = insets_.top + placement.cy * content_height();
    edit_start_distance_ = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
    edit_role_ = edit_start_distance_ > radius * 0.55f ? Role::EditResize : Role::EditMove;
    return true;
}

bool TouchOverlay::edit_move(int pointer, float x, float y) {
    if (!editing_ || pointer != edit_pointer_ || edit_control_ == TouchControl::Count) return false;
    TouchPlacement &placement = this->placement(edit_control_);
    if (edit_role_ == Role::EditMove) {
        placement.cx = edit_start_cx_ + (x - edit_start_x_) / std::max(content_width(), 1.0f);
        placement.cy = edit_start_cy_ + (y - edit_start_y_) / std::max(content_height(), 1.0f);
    } else if (edit_role_ == Role::EditResize) {
        const float cx = insets_.left + placement.cx * content_width();
        const float cy = insets_.top + placement.cy * content_height();
        const float distance = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        const float delta = (distance - edit_start_distance_) / std::max(min_dimension(), 1.0f);
        placement.radius = edit_start_radius_ + delta;
    }
    clamp_placement(placement);
    return true;
}

void TouchOverlay::edit_up(int pointer) {
    if (pointer != edit_pointer_) return;
    edit_pointer_ = -1;
    edit_role_ = Role::None;
}

void TouchOverlay::move(TouchControl control, float dx, float dy) {
    TouchPlacement &placement = this->placement(control);
    placement.cx += dx / std::max(content_width(), 1.0f);
    placement.cy += dy / std::max(content_height(), 1.0f);
    clamp_placement(placement);
}

void TouchOverlay::resize(TouchControl control, float delta_radius) {
    TouchPlacement &placement = this->placement(control);
    placement.radius = std::clamp(placement.radius + delta_radius, kMinRadius, kMaxRadius);
    clamp_placement(placement);
}

void TouchOverlay::clamp_placement(TouchPlacement &placement) const noexcept {
    placement.radius = std::clamp(placement.radius, kMinRadius, kMaxRadius);
    if (min_dimension() <= 0.0f) return;
    const float radius = placement.radius * min_dimension();
    const float width = content_width();
    const float height = content_height();
    const float margin_x = std::min(0.5f, radius / std::max(width, 1.0f));
    const float margin_y = std::min(0.5f, radius / std::max(height, 1.0f));
    placement.cx = std::clamp(placement.cx, margin_x, 1.0f - margin_x);
    placement.cy = std::clamp(placement.cy, margin_y, 1.0f - margin_y);
}

void TouchOverlay::clamp_all() noexcept {
    for (TouchPlacement &placement : placements_) clamp_placement(placement);
}

void TouchOverlay::reset_layout() {
    placements_ = default_placements();
    clamp_all();
    release_all();
}

std::string TouchOverlay::to_json() const {
    nlohmann::json root;
    root["version"] = kLayoutVersion;
    nlohmann::json controls = nlohmann::json::array();
    for (const TouchPlacement &placement : placements_) {
        controls.push_back({{"control", control_name(placement.control)},
            {"cx", placement.cx},
            {"cy", placement.cy},
            {"radius", placement.radius},
            {"visible", placement.visible}});
    }
    root["controls"] = std::move(controls);
    return root.dump();
}

TouchOverlay::LoadStatus TouchOverlay::load_json(std::string_view text) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::exception &) {
        return LoadStatus::Corrupt;
    }
    if (!root.is_object() || !root.contains("version") || !root["version"].is_number_integer())
        return LoadStatus::Corrupt;
    const int version = root["version"].get<int>();
    if (version < 1) return LoadStatus::Older;
    if (version > kLayoutVersion) return LoadStatus::Corrupt;
    if (!root.contains("controls") || !root["controls"].is_array())
        return LoadStatus::Corrupt;
    const std::size_t expected_controls = version >= 2 ? kTouchControls : kLayoutV1Controls;
    if (root["controls"].size() != expected_controls) return LoadStatus::Corrupt;

    auto candidate = placements_;
    std::array<bool, kTouchControls> seen{};
    for (const nlohmann::json &entry : root["controls"]) {
        if (!entry.is_object() || !entry.contains("control") || !entry["control"].is_string())
            return LoadStatus::Corrupt;
        TouchControl control = TouchControl::Count;
        if (!control_from_name(entry["control"].get<std::string>(), control))
            return LoadStatus::Corrupt;
        const std::size_t index = static_cast<std::size_t>(control);
        if (seen[index]) return LoadStatus::Corrupt;
        seen[index] = true;
        if (!entry.contains("cx") || !entry.contains("cy") || !entry.contains("radius") ||
            !entry.contains("visible") || !entry["cx"].is_number() || !entry["cy"].is_number() ||
            !entry["radius"].is_number() || !entry["visible"].is_boolean())
            return LoadStatus::Corrupt;
        TouchPlacement placement = candidate[index];
        placement.cx = entry["cx"].get<float>();
        placement.cy = entry["cy"].get<float>();
        placement.radius = entry["radius"].get<float>();
        placement.visible = entry["visible"].get<bool>();
        if (!std::isfinite(placement.cx) || !std::isfinite(placement.cy) ||
            !std::isfinite(placement.radius) || placement.radius <= 0.0f)
            return LoadStatus::Corrupt;
        candidate[index] = placement;
    }
    for (std::size_t i = 0; i < kTouchControls; ++i) {
        if (seen[i]) continue;
        // A version 1 layout predates fast-forward and cannot carry it;
        // every control that version knew about must still be present.
        if (version == 1 && static_cast<TouchControl>(i) == TouchControl::FastForward) continue;
        return LoadStatus::Corrupt;
    }
    placements_ = candidate;
    clamp_all();
    return LoadStatus::Ok;
}

void TouchOverlay::publish() const {
    state().set_buttons(Source::Touch, 0, 0, buttons());
    state().set_analog(Source::Touch, 0, 0, analog());
}

TouchOverlay &touch_overlay() noexcept {
    static TouchOverlay overlay;
    return overlay;
}

void reset_touch_overlay() {
    touch_overlay() = TouchOverlay{};
}

} // namespace gungnir::input

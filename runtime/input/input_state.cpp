#include "input/input_state.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace gungnir::input {
namespace {

constexpr std::array<Source, kSources> kAnalogPriority{
    Source::Touch, Source::Gamepad, Source::Keyboard, Source::Mouse};

bool source_engaged(const Analog &analog) noexcept {
    return analog.x != 0.0f || analog.y != 0.0f;
}

} // namespace

std::uint8_t axis_to_byte(float value, float deadzone) noexcept {
    if (!std::isfinite(value)) return 0x80u;
    const float clamped = std::clamp(value, -1.0f, 1.0f);
    if (clamped <= -1.0f) return 0u;
    if (clamped >= 1.0f) return 255u;
    const float magnitude = std::fabs(clamped);
    if (magnitude <= deadzone) return 0x80u;
    const float limited = std::clamp(deadzone, 0.0f, 0.999f);
    const float scaled = (magnitude - limited) / (1.0f - limited);
    const float signed_value = std::copysign(scaled, clamped);
    const long quantized = std::lround(128.0f + signed_value * 127.5f);
    return static_cast<std::uint8_t>(std::clamp<long>(quantized, 0, 255));
}

PadState to_pad(ButtonMask buttons, Analog analog, float deadzone) noexcept {
    PadState pad;
    pad.buttons = buttons;
    pad.lx = axis_to_byte(analog.x, deadzone);
    pad.ly = axis_to_byte(analog.y, deadzone);
    return pad;
}

InputState::Slot *InputState::find(Source source, int device, int pointer) noexcept {
    for (Slot &slot : slots_) {
        if (slot.used && slot.source == source && slot.device == device && slot.pointer == pointer)
            return &slot;
    }
    return nullptr;
}

const InputState::Slot *InputState::find(Source source, int device, int pointer) const noexcept {
    for (const Slot &slot : slots_) {
        if (slot.used && slot.source == source && slot.device == device && slot.pointer == pointer)
            return &slot;
    }
    return nullptr;
}

InputState::Slot &InputState::claim(Source source, int device, int pointer) noexcept {
    if (Slot *existing = find(source, device, pointer)) return *existing;
    Slot *chosen = nullptr;
    for (Slot &slot : slots_) {
        if (!slot.used) {
            chosen = &slot;
            break;
        }
    }
    if (chosen == nullptr) {
        chosen = &slots_.front();
        for (Slot &slot : slots_) {
            if (slot.revision < chosen->revision) chosen = &slot;
        }
    }
    *chosen = Slot{};
    chosen->used = true;
    chosen->source = source;
    chosen->device = device;
    chosen->pointer = pointer;
    return *chosen;
}

void InputState::set_buttons(Source source, int device, int pointer, ButtonMask buttons) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot &slot = claim(source, device, pointer);
    slot.buttons = buttons;
    slot.revision = ++revision_;
    refresh_edges();
}

void InputState::set_analog(Source source, int device, int pointer, Analog analog) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot &slot = claim(source, device, pointer);
    slot.analog = Analog{std::clamp(analog.x, -1.0f, 1.0f), std::clamp(analog.y, -1.0f, 1.0f)};
    slot.revision = ++revision_;
    refresh_edges();
}

void InputState::release(Source source, int device, int pointer) {
    std::lock_guard<std::mutex> lock(mutex_);
    Slot *slot = find(source, device, pointer);
    if (slot == nullptr) return;
    slot->used = false;
    refresh_edges();
}

void InputState::release_source(Source source) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool changed = false;
    for (Slot &slot : slots_) {
        if (slot.used && slot.source == source) {
            slot.used = false;
            changed = true;
        }
    }
    if (changed) refresh_edges();
}

void InputState::release_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Slot &slot : slots_) slot.used = false;
    refresh_edges();
}

Analog InputState::analog() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return analog_locked();
}

Analog InputState::analog_locked() const noexcept {
    for (const Source source : kAnalogPriority) {
        const Slot *best = nullptr;
        for (const Slot &slot : slots_) {
            if (!slot.used || slot.source != source || !source_engaged(slot.analog)) continue;
            if (best == nullptr || slot.revision > best->revision) best = &slot;
        }
        if (best != nullptr) return best->analog;
    }
    return {};
}

PadState InputState::pad() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return to_pad(merged_, analog_locked());
}

bool InputState::source_active(Source source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const Slot &slot : slots_) {
        if (!slot.used || slot.source != source) continue;
        if (slot.buttons != 0u || source_engaged(slot.analog)) return true;
    }
    return false;
}

ButtonMask InputState::buttons() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return merged_;
}

bool InputState::held(Button button) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (merged_ & button_mask(button)) != 0u;
}

Edges InputState::take_edges() {
    std::lock_guard<std::mutex> lock(mutex_);
    const Edges result = edges_;
    edges_ = {};
    return result;
}

void InputState::clear_edges() {
    std::lock_guard<std::mutex> lock(mutex_);
    edges_ = {};
}

void InputState::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_ = {};
    merged_ = 0u;
    edges_ = {};
    revision_ = 0u;
}

void InputState::refresh_edges() noexcept {
    ButtonMask next = 0u;
    for (const Slot &slot : slots_) {
        if (slot.used) next |= slot.buttons;
    }
    edges_.pressed |= next & ~merged_;
    edges_.released |= merged_ & ~next;
    merged_ = next;
}

InputState &state() noexcept {
    static InputState instance;
    return instance;
}

void reset_state() {
    state().reset();
}

} // namespace gungnir::input

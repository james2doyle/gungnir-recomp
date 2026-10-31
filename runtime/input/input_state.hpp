#pragma once

// Source-neutral input state (T11a).
//
// Physical sources (touch overlay, gamepad, keyboard, mouse) publish buttons
// and one analog pair into slots keyed by (source, device, pointer). The
// merged view deliberately has no SDL, JNI or guest-memory dependency so the
// merge, dead-zone and edge contracts run on the host.
//
// Merge policy:
// - Buttons are OR-ed: a button is held while any source slot holds it.
// - Analog is taken from the highest-priority engaged slot in the fixed order
//   Touch, Gamepad, Keyboard, Mouse. Within one source the most recently
//   updated slot wins. Engagement is a non-zero axis pair, so a centred touch
//   stick exposes the gamepad underneath it.
// - Axes are clamped to [-1, 1] and the dead zone is applied after the merge.
//
// Edges are accumulated per merged state and consumed by take_edges(), which
// is a single-consumer polling API (the host UI/haptics layer). The sceCtrl
// latch keeps its own previous-button copy so a guest read never races a
// host poll.

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace gungnir::input {

enum class Source : std::uint8_t {
    Touch = 0,
    Gamepad,
    Keyboard,
    Mouse,
    Count,
};
inline constexpr std::size_t kSources = static_cast<std::size_t>(Source::Count);

// PSP SceCtrlButtons values.
enum class Button : std::uint32_t {
    Select = 0x000001u,
    Start = 0x000008u,
    Up = 0x000010u,
    Right = 0x000020u,
    Down = 0x000040u,
    Left = 0x000080u,
    L = 0x000100u,
    R = 0x000200u,
    Triangle = 0x001000u,
    Circle = 0x002000u,
    Cross = 0x004000u,
    Square = 0x008000u,
    Home = 0x010000u,
    Hold = 0x020000u,
    Note = 0x800000u,
};

using ButtonMask = std::uint32_t;

[[nodiscard]] constexpr ButtonMask button_mask(Button button) noexcept {
    return static_cast<ButtonMask>(button);
}

inline constexpr ButtonMask kUserButtons = button_mask(Button::Select) | button_mask(Button::Start) |
    button_mask(Button::Up) | button_mask(Button::Right) | button_mask(Button::Down) |
    button_mask(Button::Left) | button_mask(Button::L) | button_mask(Button::R) |
    button_mask(Button::Triangle) | button_mask(Button::Circle) | button_mask(Button::Cross) |
    button_mask(Button::Square) | button_mask(Button::Home) | button_mask(Button::Hold) |
    button_mask(Button::Note);

struct Analog {
    float x{};
    float y{};

    friend bool operator==(const Analog &, const Analog &) = default;
};

// PSP analog byte: centre 0x80, right/down positive.
inline constexpr float kDefaultDeadZone = 0.12f;

[[nodiscard]] std::uint8_t axis_to_byte(float value, float deadzone = kDefaultDeadZone) noexcept;

struct PadState {
    ButtonMask buttons{};
    std::uint8_t lx{0x80u};
    std::uint8_t ly{0x80u};
    std::uint8_t rx{0x80u};
    std::uint8_t ry{0x80u};
};

[[nodiscard]] PadState to_pad(
    ButtonMask buttons, Analog analog, float deadzone = kDefaultDeadZone) noexcept;

struct Edges {
    ButtonMask pressed{};
    ButtonMask released{};

    friend bool operator==(const Edges &, const Edges &) = default;
};

class InputState {
public:
    // Replaces the slot's button mask. A pointer that slides between controls
    // publishes its new mask; releasing the old control is implicit.
    void set_buttons(Source source, int device, int pointer, ButtonMask buttons);
    // Replaces the slot's analog pair, each axis in [-1, 1].
    void set_analog(Source source, int device, int pointer, Analog analog);
    // Drops one slot (a finger lifted, a pad disconnected).
    void release(Source source, int device, int pointer);
    // Drops every slot from one source.
    void release_source(Source source);
    // Drops every source (window focus lost, app backgrounded).
    void release_all();

    [[nodiscard]] ButtonMask buttons() const;
    [[nodiscard]] bool held(Button button) const;
    [[nodiscard]] Analog analog() const;
    [[nodiscard]] PadState pad() const;
    [[nodiscard]] bool source_active(Source source) const;

    // Press/release edges since the previous take. Consuming edges does not
    // change the held state.
    [[nodiscard]] Edges take_edges();
    void clear_edges();

    // Clears every slot and the edge history.
    void reset();

private:
    struct Slot {
        bool used{};
        Source source{Source::Touch};
        int device{};
        int pointer{};
        ButtonMask buttons{};
        Analog analog{};
        std::uint64_t revision{};
    };

    static constexpr std::size_t kMaxSlots = 16u;

    [[nodiscard]] Slot *find(Source source, int device, int pointer) noexcept;
    [[nodiscard]] const Slot *find(Source source, int device, int pointer) const noexcept;
    Slot &claim(Source source, int device, int pointer) noexcept;
    void refresh_edges() noexcept;
    [[nodiscard]] Analog analog_locked() const noexcept;

    mutable std::mutex mutex_;
    std::array<Slot, kMaxSlots> slots_{};
    ButtonMask merged_{};
    Edges edges_{};
    std::uint64_t revision_{};
};

// Process-wide state. Host input producers and the sceCtrl HLE share it; the
// event loop and HLE run on the same thread.
[[nodiscard]] InputState &state() noexcept;

// Clears the global state and edge history. Fixtures call it between cases;
// the boot runner starts from a fresh process.
void reset_state();

} // namespace gungnir::input

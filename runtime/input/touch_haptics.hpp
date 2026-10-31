#pragma once

// Touch-control haptic policy (T11c).
//
// The policy is host-testable and has no Android dependency. A sink is the
// actuator (the Android View supplies `performHapticFeedback`); it is called
// exactly once for a new face-button, L/R, START/SELECT or d-pad touch-down.
// The stick, a slide or move, edit gestures and release never call it.
//
// There is deliberately no time-based throttle: swallowing a genuine rapid
// press is worse than asking the platform twice. Android's haptic service
// coalesces at the hardware layer.

#include "input/touch_overlay.hpp"

#include <functional>

namespace gungnir::input {

class TouchHaptics {
public:
    using Sink = std::function<void()>;

    explicit TouchHaptics(Sink sink = {});
    TouchHaptics(const TouchHaptics &) = delete;
    TouchHaptics &operator=(const TouchHaptics &) = delete;

    void set_sink(Sink sink);
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    [[nodiscard]] static bool is_haptic_control(TouchControl control) noexcept;

    // Requests one tick. Returns true only when the sink was called and did
    // not throw. A missing/disabled sink or an empty hit is a no-op.
    [[nodiscard]] bool press(TouchControl control);

private:
    Sink sink_;
    bool enabled_{true};
};

} // namespace gungnir::input

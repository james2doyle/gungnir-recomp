#include "ge/present_schedule.hpp"

#include <algorithm>
#include <cmath>

namespace gungnir::ge {

double cap_refresh_hz(double hz) noexcept {
    if (!std::isfinite(hz) || hz <= 0.0) return kMaxPresentHz;
    return std::min(hz, kMaxPresentHz);
}

void PresentSchedule::set_refresh_interval(Clock::duration interval) noexcept {
    refresh_ = interval > Clock::duration::zero() ? interval : std::chrono::microseconds(1);
}

bool PresentSchedule::on_guest_frame(Clock::time_point now, bool has_previous,
    bool static_scene) noexcept {
    if (!enabled_) {
        // Nothing to interpolate from: the frame goes out as it is.
        active_ = false;
        return true;
    }
    if (!has_previous) {
        // Nothing to interpolate from, but this arrival still dates the
        // interval the next window will use.
        window_start_ = now;
        has_window_ = true;
        active_ = false;
        return true;
    }
    if (static_scene) {
        // A frame identical to its predecessor has no motion to blend. Show the
        // real frame once and close any window: no in-between presents, no
        // interpolate_frame passes, no driving the 120 Hz display for a still
        // picture. The window_start_ still dates the next interval so a later
        // moving frame opens a correctly-sized window.
        window_start_ = now;
        has_window_ = true;
        active_ = false;
        return true;
    }
    // Ticks the closing window still owed are refreshes the presenter missed.
    if (active_ && now > next_tick_) {
        skipped_ += 1u + static_cast<std::uint64_t>((now - next_tick_) / refresh_);
    }
    // The previous guest interval is the best estimate of this one, so the
    // window is exactly as long as the motion it shows.
    window_ = has_window_ ? now - window_start_ : refresh_;
    if (window_ < std::chrono::milliseconds(1)) window_ = std::chrono::milliseconds(1);
    window_start_ = now;
    next_tick_ = now + refresh_;
    active_ = true;
    has_window_ = true;
    return false;
}

PresentSchedule::Due PresentSchedule::due(Clock::time_point now) noexcept {
    Due due;
    if (!enabled_ || !active_ || now < next_tick_) return due;
    const double elapsed =
        std::chrono::duration<double>(now - window_start_).count();
    const double span = std::chrono::duration<double>(window_).count();
    const double t = std::clamp(elapsed / span, 0.0, 1.0);
    if (t >= 1.0) {
        // The next guest frame is late: stop here and let the caller show the
        // real frame rather than extrapolating past what the guest produced.
        active_ = false;
        ++held_;
        due.kind = Kind::Held;
        return due;
    }
    due.kind = Kind::Interpolated;
    due.t = static_cast<float>(t);
    ++interpolated_;
    // Count every refresh that went by without a present, not just the one the
    // tick landed on, and move straight to the next future tick: never burst
    // to catch up.
    const auto behind = now - next_tick_;
    const auto missed = static_cast<std::uint64_t>(behind / refresh_);
    if (missed != 0u) {
        due.late_tick = true;
        skipped_ += missed;
    }
    next_tick_ = now + refresh_;
    return due;
}

void PresentSchedule::reset() noexcept {
    active_ = false;
    has_window_ = false;
    window_start_ = {};
    window_ = std::chrono::milliseconds(1);
    next_tick_ = {};
    interpolated_ = 0u;
    held_ = 0u;
    skipped_ = 0u;
}

} // namespace gungnir::ge

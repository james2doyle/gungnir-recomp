#pragma once

// Display schedule for in-between frames (T17a.6).
//
// In-betweens can only be built backwards: at the moment frame N arrives, the
// motion from N-1 to N is known, and it is drawn during the *next* guest
// interval. That costs one guest interval of display latency by construction;
// this class owns only the timing, never the frames.
//
// Pure and time-injected so the contracts run on the host: a window per guest
// frame, one present per display refresh inside it, the real frame at the end,
// and no present at all when interpolation is off.

#include <chrono>
#include <cstdint>

namespace gungnir::ge {

class PresentSchedule {
public:
    using Clock = std::chrono::steady_clock;

    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void set_refresh_interval(Clock::duration interval) noexcept;
    [[nodiscard]] Clock::duration refresh_interval() const noexcept { return refresh_; }

    // A guest frame arrived at `now`. Returns true when it must be presented
    // immediately: interpolation is off, or there is no previous frame to
    // interpolate from. Otherwise a display window starts and `due` presents
    // the in-betweens.
    bool on_guest_frame(Clock::time_point now, bool has_previous) noexcept;

    enum class Kind {
        None,
        Interpolated,  // draw the previous-to-current blend at `t`
        Held,          // the next guest frame is late: show the real frame once
    };
    struct Due {
        Kind kind{Kind::None};
        float t{};
        bool late_tick{};  // the presenter could not keep the last tick
    };
    // What to present at `now`, if anything. Advances the tick when it answers.
    Due due(Clock::time_point now) noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] Clock::time_point next_tick() const noexcept { return next_tick_; }

    void reset() noexcept;
    [[nodiscard]] std::uint64_t interpolated() const noexcept { return interpolated_; }
    [[nodiscard]] std::uint64_t held() const noexcept { return held_; }
    [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

private:
    bool enabled_{};
    Clock::duration refresh_{std::chrono::microseconds(16'667)};
    bool active_{};
    bool has_window_{};
    Clock::time_point window_start_{};
    Clock::duration window_{std::chrono::milliseconds(1)};
    Clock::time_point next_tick_{};
    std::uint64_t interpolated_{};
    std::uint64_t held_{};
    std::uint64_t skipped_{};
};

} // namespace gungnir::ge

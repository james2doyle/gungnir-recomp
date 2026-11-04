#pragma once

// Host execution controls (fast-forward).
//
// These are host-side switches, not guest input: they never reach sceCtrl and
// never enter a snapshot. Producers (the SDL gamepad adapter, the touch
// overlay, the Android shell) publish them; the kernel's pacing hold and the
// audio mixer consume them.
//
// A control is active while it is held (a momentary source such as the R2
// trigger) or latched (a toggle such as the on-screen FF button). Releasing a
// hold leaves a latch alone, and clearing the last source returns to 1x.
//
// The state is process-wide and lock-free: the Android View publishes from the
// UI thread while the emulation thread reads it on every pacing step.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace gungnir::input {

enum class HostControl : std::uint8_t {
    FastForward = 0,
    Count,
};

inline constexpr std::size_t kHostControls = static_cast<std::size_t>(HostControl::Count);
// Emulated time advances this many times faster than real time.
inline constexpr double kFastForwardSpeed = 3.0;
inline constexpr double kNominalSpeed = 1.0;

class HostControls {
public:
    void hold(HostControl control, bool held) noexcept;
    void latch(HostControl control, bool latched) noexcept;
    void toggle(HostControl control) noexcept;
    [[nodiscard]] bool held(HostControl control) const noexcept;
    [[nodiscard]] bool latched(HostControl control) const noexcept;
    [[nodiscard]] bool active(HostControl control) const noexcept;
    // Drops every momentary source, as on focus loss; latches stay.
    void release_holds() noexcept;
    void reset() noexcept;
    // The pacing multiple the active controls ask for.
    [[nodiscard]] double speed() const noexcept;

private:
    std::atomic<std::uint32_t> held_{0};
    std::atomic<std::uint32_t> latched_{0};
};

// Process-wide instance shared by the host producers and the pacing hold.
[[nodiscard]] HostControls &host_controls() noexcept;
void reset_host_controls();

} // namespace gungnir::input

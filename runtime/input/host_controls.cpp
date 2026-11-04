#include "input/host_controls.hpp"

namespace gungnir::input {
namespace {

constexpr std::uint32_t control_bit(HostControl control) noexcept {
    return 1u << static_cast<std::uint32_t>(control);
}

} // namespace

void HostControls::hold(HostControl control, bool value) noexcept {
    const std::uint32_t mask = control_bit(control);
    if (value) {
        held_.fetch_or(mask, std::memory_order_release);
    } else {
        held_.fetch_and(~mask, std::memory_order_release);
    }
}

void HostControls::latch(HostControl control, bool value) noexcept {
    const std::uint32_t mask = control_bit(control);
    if (value) {
        latched_.fetch_or(mask, std::memory_order_release);
    } else {
        latched_.fetch_and(~mask, std::memory_order_release);
    }
}

void HostControls::toggle(HostControl control) noexcept {
    latched_.fetch_xor(control_bit(control), std::memory_order_release);
}

bool HostControls::held(HostControl control) const noexcept {
    return (held_.load(std::memory_order_acquire) & control_bit(control)) != 0u;
}

bool HostControls::latched(HostControl control) const noexcept {
    return (latched_.load(std::memory_order_acquire) & control_bit(control)) != 0u;
}

bool HostControls::active(HostControl control) const noexcept {
    const std::uint32_t mask = control_bit(control);
    return ((held_.load(std::memory_order_acquire) | latched_.load(std::memory_order_acquire)) &
               mask) != 0u;
}

void HostControls::release_holds() noexcept {
    held_.store(0u, std::memory_order_release);
}

void HostControls::reset() noexcept {
    held_.store(0u, std::memory_order_release);
    latched_.store(0u, std::memory_order_release);
}

double HostControls::speed() const noexcept {
    return active(HostControl::FastForward) ? kFastForwardSpeed : kNominalSpeed;
}

HostControls &host_controls() noexcept {
    static HostControls instance;
    return instance;
}

void reset_host_controls() {
    host_controls().reset();
}

} // namespace gungnir::input

// Fast-forward host-control contracts: the hold/latch merge, the speed it
// reports and the ways it is cleared. The control is host execution policy,
// so nothing here reaches sceCtrl or a snapshot.

#include "input/host_controls.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using gungnir::input::HostControl;
using gungnir::input::host_controls;
using gungnir::input::kFastForwardSpeed;
using gungnir::input::reset_host_controls;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

bool fast_forward() {
    return host_controls().active(HostControl::FastForward);
}

void idle_by_default() {
    reset_host_controls();
    check(!fast_forward(), "fast-forward starts off");
    check(host_controls().speed() == 1.0, "idle speed is nominal");
}

void hold_is_momentary() {
    reset_host_controls();
    host_controls().hold(HostControl::FastForward, true);
    check(fast_forward(), "a held trigger turns fast-forward on");
    check(host_controls().speed() == kFastForwardSpeed, "a held trigger runs faster than nominal");
    host_controls().hold(HostControl::FastForward, false);
    check(!fast_forward(), "releasing the trigger returns to 1x");
    check(host_controls().speed() == 1.0, "released speed is nominal");
}

void latch_survives_release() {
    reset_host_controls();
    host_controls().toggle(HostControl::FastForward);
    check(fast_forward(), "toggling latches fast-forward on");
    host_controls().release_holds();
    check(fast_forward(), "releasing holds does not clear a latch");
    host_controls().toggle(HostControl::FastForward);
    check(!fast_forward(), "toggling again returns to 1x");
}

void sources_are_independent() {
    reset_host_controls();
    host_controls().hold(HostControl::FastForward, true);
    host_controls().latch(HostControl::FastForward, true);
    host_controls().hold(HostControl::FastForward, false);
    check(fast_forward(), "a latch keeps fast-forward on after the hold ends");
    host_controls().latch(HostControl::FastForward, false);
    check(!fast_forward(), "clearing the last source returns to 1x");
}

void reset_clears_every_source() {
    reset_host_controls();
    host_controls().hold(HostControl::FastForward, true);
    host_controls().latch(HostControl::FastForward, true);
    reset_host_controls();
    check(!fast_forward(), "a reset clears the hold and the latch");
    check(host_controls().speed() == 1.0, "a reset restores nominal speed");
}

} // namespace

int main() {
    try {
        idle_by_default();
        hold_is_momentary();
        latch_survives_release();
        sources_are_independent();
        reset_clears_every_source();
    } catch (const std::exception &error) {
        std::cerr << "host_controls_tests: FAILED: " << error.what() << "\n";
        return 1;
    }
    std::cout << "host_controls_tests: OK\n";
    return 0;
}

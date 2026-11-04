// Android background/foreground policy contracts for the guest session.
//
// The OS takes the surface away and may keep the process: the guest is stopped
// between dispatches on background and relaunched from the saved context on
// foreground, so a round trip never ends play. This is the decision half of
// that policy — it must never revive a run that ended on its own (a bounded
// QA run, a dispatch limit or a crash) and it must never fire the resume while
// the worker is still winding down.
//
// Red-first: written before runtime/session/session_lifecycle.* existed.

#include "session/session_lifecycle.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using Action = gungnir::session::SessionLifecycle::Action;
using gungnir::session::SessionLifecycle;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

void background_stops_a_live_session() {
    SessionLifecycle policy;
    check(!policy.paused(), "a fresh policy holds no pause");
    check(policy.on_background(true) == Action::Stop,
          "backgrounding a live session requests a stop");
    check(policy.paused(), "the pause is remembered for the foreground event");
}

void background_ignores_a_finished_session() {
    SessionLifecycle policy;
    check(policy.on_background(false) == Action::None,
          "a finished session is not stopped a second time");
    check(!policy.paused(), "a finished session leaves nothing to resume");
    check(policy.on_foreground(true, true) == Action::None,
          "a finished session is never revived by a foreground event");
}

void repeated_background_stops_once() {
    SessionLifecycle policy;
    check(policy.on_background(true) == Action::Stop, "the first background stops the guest");
    check(policy.on_background(true) == Action::None,
          "a repeated background event does not stop an already paused guest");
    check(policy.paused(), "the repeated event keeps the pause pending");
}

void foreground_without_background_does_nothing() {
    SessionLifecycle policy;
    check(policy.on_foreground(true, true) == Action::None,
          "a foreground event without a background does nothing");
    check(policy.on_foreground(false, true) == Action::None,
          "an unpaused policy ignores a running worker");
}

void foreground_waits_for_the_worker() {
    SessionLifecycle policy;
    policy.on_background(true);
    check(policy.on_foreground(false, true) == Action::None,
          "the worker has not returned yet, so nothing resumes");
    check(policy.paused(), "the pause stays pending until the worker returns");
    check(policy.on_foreground(true, true) == Action::Resume,
          "the returned worker resumes in place");
    check(!policy.paused(), "a resumed pause is cleared");
    check(policy.on_foreground(true, true) == Action::None,
          "a second foreground event does not resume twice");
}

void foreground_does_not_revive_an_own_reason_stop() {
    SessionLifecycle policy;
    policy.on_background(true);
    check(policy.on_foreground(true, false) == Action::None,
          "a run that hit its wall-clock or dispatch bound is not revived");
    check(!policy.paused(), "an own-reason stop leaves nothing pending");
}

void reset_clears_a_pending_pause() {
    SessionLifecycle policy;
    policy.on_background(true);
    policy.reset();
    check(!policy.paused(), "a menu relaunch clears the pending pause");
    check(policy.on_foreground(true, true) == Action::None,
          "a cleared pause never resumes twice");
    check(policy.on_background(true) == Action::Stop,
          "the policy still stops the guest after a reset");
}

} // namespace

int main() {
    try {
        background_stops_a_live_session();
        background_ignores_a_finished_session();
        repeated_background_stops_once();
        foreground_without_background_does_nothing();
        foreground_waits_for_the_worker();
        foreground_does_not_revive_an_own_reason_stop();
        reset_clears_a_pending_pause();
        std::cout << "session_lifecycle_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

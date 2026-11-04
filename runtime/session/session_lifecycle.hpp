#pragma once

namespace gungnir::session {

// Host policy for the Android activity lifecycle. The OS takes the surface
// away on background and may keep the process: the guest is stopped between
// dispatches and relaunched from its saved context when the activity comes
// back, so a round trip keeps progress and costs no CPU behind another app.
//
// This type only decides; the owner performs the stop and the relaunch.
class SessionLifecycle {
public:
    enum class Action {
        None,
        Stop,
        Resume,
    };

    // A live session is stopped once. A finished session stays finished: a
    // bounded run that hit its dispatch or wall-clock limit, or one that
    // failed, must never be revived by an activity round trip.
    [[nodiscard]] Action on_background(bool session_live) noexcept;

    // `session_finished` is the worker's report and `stopped_by_host` the
    // recorded stop cause. Until the worker has actually returned the pause
    // stays pending and the action is None; a run that ended for its own
    // reason is dropped rather than resumed.
    [[nodiscard]] Action on_foreground(bool session_finished, bool stopped_by_host) noexcept;

    [[nodiscard]] bool paused() const noexcept { return paused_; }

    // A menu save/load/reset relaunch makes a pending pause moot (that path
    // starts its own worker), so the owner clears it rather than resuming on
    // top of a live session.
    void reset() noexcept { paused_ = false; }

private:
    bool paused_{false};
};

} // namespace gungnir::session

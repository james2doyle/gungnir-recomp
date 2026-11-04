#include "session/session_lifecycle.hpp"

namespace gungnir::session {

SessionLifecycle::Action SessionLifecycle::on_background(bool session_live) noexcept {
    if (!session_live) return Action::None;
    if (paused_) return Action::None;
    paused_ = true;
    return Action::Stop;
}

SessionLifecycle::Action SessionLifecycle::on_foreground(
    bool session_finished, bool stopped_by_host) noexcept {
    if (!paused_) return Action::None;
    if (!session_finished) return Action::None;
    paused_ = false;
    return stopped_by_host ? Action::Resume : Action::None;
}

} // namespace gungnir::session

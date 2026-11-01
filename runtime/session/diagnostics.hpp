#pragma once

// Opt-in session diagnostics. The chained-call ring records the native
// `invoke_chained_call` frames (target, depth, return PC, a0-a3) so a fault
// deep inside one outer dispatch can be attributed to its callers. The ring is
// disabled (capacity 0) unless a caller enables it.

#include <cstddef>
#include <cstdint>
#include <iosfwd>

namespace gungnir::session {

void configure_chain_ring(std::size_t capacity);
[[nodiscard]] std::size_t chain_ring_capacity();
void chain_ring_enter(std::uint32_t target, std::uint32_t depth, std::uint32_t ra,
    std::uint32_t a0, std::uint32_t a1, std::uint32_t a2, std::uint32_t a3, const char *thread);
void chain_ring_leave(std::uint32_t target, std::uint32_t depth, std::uint32_t v0, std::uint32_t v1);
void dump_chain_ring(std::ostream &out);

// Guest-progress watchdog. A guest thread that blocks inside a host wait never
// reaches the scheduler, so neither the dispatch limit nor the wall clock can
// report it; this notices a progress counter that stops moving. Time is
// injected so the policy is testable without sleeping.
class StallWatchdog {
public:
    explicit StallWatchdog(double limit_seconds) noexcept;

    // Returns true exactly once when `progress` has not changed for longer
    // than the limit. Any change re-arms it.
    bool observe(std::uint64_t progress, double now_seconds) noexcept;

    [[nodiscard]] double stalled_seconds() const noexcept;
    [[nodiscard]] bool enabled() const noexcept { return limit_seconds_ > 0.0; }

private:
    double limit_seconds_{};
    std::uint64_t last_progress_{};
    double last_change_{};
    double stalled_{};
    bool seen_{};
    bool reported_{};
};

} // namespace gungnir::session

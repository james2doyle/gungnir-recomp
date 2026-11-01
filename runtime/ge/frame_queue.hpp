#pragma once

// Latest-frame handoff between the guest session thread and the presentation
// thread (T13b). Publishing replaces any unrendered frame; the drop is counted.

#include "ge/state_machine.hpp"

#include <cstdint>
#include <mutex>
#include <vector>

namespace gungnir::ge {

class FrameQueue {
public:
    void publish(std::vector<Draw> draws);
    bool take_latest(std::vector<Draw> &draws);

    [[nodiscard]] std::uint64_t published() const;
    [[nodiscard]] std::uint64_t dropped() const;

private:
    mutable std::mutex mutex_;
    std::vector<Draw> latest_;
    bool has_frame_{};
    std::uint64_t published_{};
    std::uint64_t dropped_{};
};

} // namespace gungnir::ge

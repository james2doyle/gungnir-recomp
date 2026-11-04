#pragma once

#include <cstddef>

namespace gungnir::platform {

// How much of the calling thread's stack is already in use.
//
// The Android shell runs its runtime on threads whose stack it does not
// choose (the SDL thread created by SDLActivity's Java thread defaults, and
// the session worker's std::thread). Reporting the real numbers at startup
// names the margin instead of assuming one, so a later frame that grows into
// it is visible before it overflows.
struct StackSpace {
    std::size_t limit_bytes{};
    std::size_t used_bytes{};
    std::size_t headroom_bytes{};
    bool known{};
};

StackSpace current_stack_space();

} // namespace gungnir::platform

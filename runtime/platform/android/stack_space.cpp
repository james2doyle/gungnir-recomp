#include "platform/android/stack_space.hpp"

#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace gungnir::platform {
namespace {

std::uintptr_t stack_pointer() {
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

} // namespace

StackSpace current_stack_space() {
    StackSpace space;
#if defined(__linux__)
    pthread_attr_t attributes;
    if (pthread_getattr_np(pthread_self(), &attributes) != 0) return space;
    void *base = nullptr;
    std::size_t size = 0;
    const int queried = pthread_attr_getstack(&attributes, &base, &size);
    pthread_attr_destroy(&attributes);
    if (queried != 0 || base == nullptr || size == 0) return space;
    const std::uintptr_t low = reinterpret_cast<std::uintptr_t>(base);
    const std::uintptr_t high = low + size;
    const std::uintptr_t here = stack_pointer();
    // A thread whose descriptor cannot be read is reported as unknown rather
    // than as a made-up margin.
    if (here < low || here >= high) return space;
    space.limit_bytes = size;
    space.used_bytes = static_cast<std::size_t>(high - here);
    space.headroom_bytes = size - space.used_bytes;
    space.known = true;
#endif
    return space;
}

} // namespace gungnir::platform

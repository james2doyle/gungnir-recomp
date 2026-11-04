// Startup-limit contract: the runtime must be able to say how much stack the
// calling thread has left. Red-first context: the identity gate overflowed the
// SDL thread's ~1 MiB stack while this was written.

#include "platform/android/stack_space.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include <pthread.h>

namespace {

using gungnir::platform::StackSpace;
using gungnir::platform::current_stack_space;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

struct ThreadRun {
    StackSpace space;
};

void *measure_narrow_thread(void *argument) {
    auto *run = static_cast<ThreadRun *>(argument);
    run->space = current_stack_space();
    return nullptr;
}

void reports_the_calling_thread_stack() {
    const StackSpace main_space = current_stack_space();
    check(main_space.known, "the calling thread's stack is measurable");
    check(main_space.limit_bytes > 64u * 1024u, "the reported limit is a real stack");
    check(main_space.used_bytes > 0u, "the used bytes are non-zero");
    check(main_space.headroom_bytes == main_space.limit_bytes - main_space.used_bytes,
          "headroom is the limit minus the bytes in use");
    check(main_space.used_bytes < main_space.limit_bytes, "the main thread has headroom");

    constexpr std::size_t budget = 256u * 1024u;
    pthread_attr_t attributes;
    check(pthread_attr_init(&attributes) == 0, "thread attributes initialise");
    check(pthread_attr_setstacksize(&attributes, budget) == 0, "a narrow stack is accepted");
    ThreadRun run;
    pthread_t thread{};
    check(pthread_create(&thread, &attributes, measure_narrow_thread, &run) == 0,
          "the narrow thread starts");
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
    check(run.space.known, "the narrow thread's stack is measurable");
    check(run.space.limit_bytes <= budget, "the narrow thread reports its own limit");
    check(run.space.limit_bytes >= budget - 64u * 1024u,
          "the narrow thread's limit is close to what it was given");
    check(run.space.headroom_bytes < main_space.headroom_bytes,
          "a narrow stack has less headroom than the main thread");
}

std::size_t headroom_with_a_frame() {
    volatile std::uint64_t frame[4096]{};
    frame[0] = 1;
    const StackSpace space = current_stack_space();
    return space.headroom_bytes + static_cast<std::size_t>(frame[0]);
}

void headroom_follows_the_frame_depth() {
    const StackSpace space = current_stack_space();
    check(headroom_with_a_frame() < space.headroom_bytes,
          "a deeper frame is reported as less headroom");
}

} // namespace

int main() {
    try {
        reports_the_calling_thread_stack();
        headroom_follows_the_frame_depth();
        std::cout << "stack_space_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

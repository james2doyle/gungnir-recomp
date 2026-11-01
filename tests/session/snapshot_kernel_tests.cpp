// T15b slice 2 — threads + kernel objects + timers (red first).
//
// Usage: snapshot_kernel_tests
// Exit 0 on the asserted contract, 1 on a failed contract.

#include "session/snapshot.hpp"

#include "hle/kernel.hpp"
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

using gungnir::hle::Kernel;
using gungnir::hle::SceUID;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::array<std::uint8_t, 32> filled(std::uint8_t value) {
    std::array<std::uint8_t, 32> out{};
    out.fill(value);
    return out;
}

struct Env {
    psprecomp::Runtime runtime;
    Kernel kernel;
    psprecomp::AllegrexContext cpu;
    gungnir::session::ClockState clock{};

    Env() : runtime(32u * 1024u * 1024u) {
        kernel.install(runtime, 0x08801000u, 0x08820000u);
        kernel.set_pacing_enabled(false);
        clock.now_us = kernel.now_us();
        clock.next_vblank_us = kernel.next_vblank_us();
        clock.vblank_count = kernel.vblank_count();
    }

    psprecomp::GuestMemory &memory() { return runtime.memory(); }
    const psprecomp::GuestMemory &memory() const { return runtime.memory(); }
};

struct Ids {
    SceUID sema{0};
    SceUID event{0};
    SceUID mutex{0};
    SceUID vtimer{0};
    SceUID callback{0};
};

void populate(Env &env, Ids *ids = nullptr) {
    const std::int32_t t1 = env.kernel.create_thread("worker", 0x08810000u, 32u, 4096u, 0u, 0x08900000u);
    const std::int32_t t2 = env.kernel.create_thread("waiter", 0x08811000u, 33u, 4096u, 0u, 0x08900000u);
    check(t1 > 0 && t2 > 0, "threads created");
    const SceUID sema_uid = env.kernel.allocate_uid();
    const SceUID event_uid = env.kernel.allocate_uid();
    const SceUID mutex_uid = env.kernel.allocate_uid();
    const SceUID vtimer_uid = env.kernel.allocate_uid();
    const SceUID cb_uid = env.kernel.allocate_uid();
    if (ids != nullptr) {
        ids->sema = sema_uid;
        ids->event = event_uid;
        ids->mutex = mutex_uid;
        ids->vtimer = vtimer_uid;
        ids->callback = cb_uid;
    }
    if (gungnir::hle::Thread *thread = env.kernel.find_thread(t1)) {
        thread->status = gungnir::hle::ThreadStatus::Waiting;
        thread->wait.type = gungnir::hle::WaitType::Delay;
        thread->wait.deadline_us = env.clock.now_us + 5000u;
        thread->context.set_gpr(4, 0xDEADu);
    }
    if (gungnir::hle::Thread *thread = env.kernel.find_thread(t2)) {
        thread->status = gungnir::hle::ThreadStatus::Waiting;
        thread->wait.type = gungnir::hle::WaitType::Semaphore;
        thread->wait.object = sema_uid;
        thread->wait.value = 1u;
        thread->wait.deadline_us = env.clock.now_us + 9000u;
    }
    gungnir::hle::Semaphore sema;
    sema.name = "test_sema";
    sema.count = 0;
    sema.max_count = 1;
    sema.waiters.push_back(t2);
    env.kernel.semaphores[sema_uid] = sema;

    gungnir::hle::EventFlag ev;
    ev.name = "test_ev";
    ev.pattern = 0x03u;
    env.kernel.event_flags[event_uid] = ev;

    gungnir::hle::Mutex mutex;
    mutex.name = "test_mutex";
    mutex.owner = t1;
    mutex.lock_count = 1;
    env.kernel.mutexes[mutex_uid] = mutex;

    gungnir::hle::VTimer vtimer;
    vtimer.name = "test_vtimer";
    vtimer.active = true;
    vtimer.base_us = env.clock.now_us;
    vtimer.schedule_us = 16683u * 4u;
    vtimer.handler = 0x08812000u;
    env.kernel.vtimers[vtimer_uid] = vtimer;

    gungnir::hle::Callback cb;
    cb.name = "test_cb";
    cb.function = 0x08813000u;
    cb.owner = t1;
    env.kernel.callbacks[cb_uid] = cb;

    env.kernel.sub_interrupts[30u][0u] = gungnir::hle::SubInterruptHandler{0x08814000u, 0u, true};
}

} // namespace

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gungnir_snapshot_kernel_tests";
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "slice2.gsnap";
    const auto corpus = filled(0x11u);
    const auto config = filled(0x22u);

    Env env;
    Ids ids;
    populate(env, &ids);
    const std::uint64_t timer_before = env.kernel.vtimer_value(env.kernel.vtimers.at(ids.vtimer));

    gungnir::session::SnapshotError error{};
    check(gungnir::session::snapshot_save_full(
              path, corpus, config, env.memory(), env.cpu, env.clock, env.kernel, error),
        "save_full ok: " + error.message);

    Env restored;
    check(gungnir::session::snapshot_load_full(
              path, corpus, config, restored.memory(), restored.cpu, restored.clock, restored.kernel, error),
        "load_full ok: " + error.message);
    check(restored.kernel.vtimer_value(restored.kernel.vtimers.at(ids.vtimer)) == timer_before,
        "VTimer fires at same virtual time after restore");
    check(restored.kernel.semaphores.count(ids.sema) == 1u, "semaphore restored");
    check(restored.kernel.semaphores.at(ids.sema).waiters.size() == 1u, "semaphore waiter restored");
    check(restored.kernel.event_flags.count(ids.event) == 1u, "eventflag restored");
    check(restored.kernel.mutexes.at(ids.mutex).owner != 0, "mutex owner restored");
    check(restored.kernel.callbacks.count(ids.callback) == 1u, "callback restored");

    bool found_delay = false;
    bool found_sema_wait = false;
    for (const auto &[uid, thread] : restored.kernel.threads()) {
        (void)uid;
        if (thread->wait.type == gungnir::hle::WaitType::Delay && thread->wait.deadline_us &&
            *thread->wait.deadline_us == env.clock.now_us + 5000u)
            found_delay = true;
        if (thread->wait.type == gungnir::hle::WaitType::Semaphore && thread->wait.deadline_us &&
            *thread->wait.deadline_us == env.clock.now_us + 9000u)
            found_sema_wait = true;
    }
    check(found_delay, "delay deadline preserved (absolute)");
    check(found_sema_wait, "semaphore wait deadline preserved");

    {
        Env dup;
        Ids dup_ids;
        populate(dup, &dup_ids);
        dup.kernel.event_flags[dup_ids.sema] = gungnir::hle::EventFlag{};
        dup.kernel.event_flags[dup_ids.sema].name = "collision";
        const std::filesystem::path dup_path = dir / "dup.gsnap";
        const bool saved = gungnir::session::snapshot_save_full(
            dup_path, corpus, config, dup.memory(), dup.cpu, dup.clock, dup.kernel, error);
        check(!saved, "duplicate kernel UID rejected at save");
        check(error.code == gungnir::session::SnapshotError::kDuplicate, "duplicate names snapshot_duplicate_uid");
    }

    {
        Env busy;
        populate(busy);
        busy.kernel.queue_interrupt(gungnir::hle::InterruptCall{0x08815000u, {1u, 2u, 3u, 4u}, nullptr});
        const std::filesystem::path busy_path = dir / "busy.gsnap";
        const bool saved = gungnir::session::snapshot_save_full(
            busy_path, corpus, config, busy.memory(), busy.cpu, busy.clock, busy.kernel, error);
        check(!saved, "pending interrupt rejects save (quiesce first)");
        check(error.code == gungnir::session::SnapshotError::kBusy, "busy names snapshot_busy");
    }

    {
        Env host;
        populate(host);
        const std::int32_t t = host.kernel.create_thread("hostwait", 0x08816000u, 32u, 4096u, 0u, 0x08900000u);
        if (gungnir::hle::Thread *thread = host.kernel.find_thread(t)) {
            thread->status = gungnir::hle::ThreadStatus::Waiting;
            thread->wait.type = gungnir::hle::WaitType::Host;
            thread->wait.host_poll = [](bool) -> std::optional<std::uint32_t> { return std::nullopt; };
        }
        const std::filesystem::path host_path = dir / "host.gsnap";
        const bool saved = gungnir::session::snapshot_save_full(
            host_path, corpus, config, host.memory(), host.cpu, host.clock, host.kernel, error);
        check(!saved, "host wait rejects save (not serializable)");
    }

    if (failures != 0) {
        std::cerr << "snapshot_kernel_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "snapshot_kernel_tests: OK\n";
    return 0;
}

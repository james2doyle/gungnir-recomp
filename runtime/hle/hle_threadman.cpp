// ThreadManForUser and Kernel_Library: threads, time, semaphores, event flags,
// mutexes, callbacks, VTimers and interrupt masking.
#include "hle/hle_common.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kEventFlagWaitMulti = 0x200u;
constexpr std::uint32_t kMutexAttrRecursive = 0x200u;

std::int32_t as_signed(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}
std::uint32_t as_unsigned(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

void register_threads(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateThread", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        kernel().finish(ctx,
            as_unsigned(kernel().create_thread(name, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4), ctx.gpr[28])));
    });
    hle.add("ThreadManForUser", "sceKernelStartThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().start_thread(ctx, as_signed(arg(ctx, 0)), arg(ctx, 1), arg(ctx, 2))));
    });
    hle.add("ThreadManForUser", "sceKernelExitThread",
        [](Runtime &, AllegrexContext &ctx) { kernel().exit_current_thread(ctx, as_signed(arg(ctx, 0)), false); });
    hle.add("ThreadManForUser", "sceKernelExitDeleteThread",
        [](Runtime &, AllegrexContext &ctx) { kernel().exit_current_thread(ctx, as_signed(arg(ctx, 0)), true); });
    hle.add("ThreadManForUser", "sceKernelDeleteThread", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        kernel().finish(
            ctx, uid == kernel().current_uid() ? error::kNotDormant : as_unsigned(kernel().delete_thread(uid)));
    });
    hle.add("ThreadManForUser", "sceKernelTerminateThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().terminate_thread(ctx, as_signed(arg(ctx, 0)), false)));
    });
    hle.add("ThreadManForUser", "sceKernelTerminateDeleteThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().terminate_thread(ctx, as_signed(arg(ctx, 0)), true)));
    });
    const auto get_thread_id = [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().current_uid()));
    };
    hle.add("ThreadManForUser", "sceKernelGetThreadId", get_thread_id);
    hle.add("Kernel_Library", "sceKernelGetThreadId", get_thread_id);
    hle.add("ThreadManForUser", "sceKernelGetThreadCurrentPriority", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().current_thread();
        kernel().finish(ctx, thread != nullptr ? thread->priority : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelChangeThreadPriority", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, as_unsigned(kernel().change_priority(ctx, as_signed(arg(ctx, 0)), arg(ctx, 1))));
    });
    hle.add("ThreadManForUser", "sceKernelChangeCurrentThreadAttr", [](Runtime &, AllegrexContext &ctx) {
        if (Thread *thread = kernel().current_thread())
            thread->attributes = (thread->attributes & ~arg(ctx, 0)) | arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelGetThreadExitStatus", [](Runtime &, AllegrexContext &ctx) {
        const Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr)
            kernel().finish(ctx, error::kUnknownThid);
        else if (thread->status != ThreadStatus::Dormant)
            kernel().finish(ctx, error::kNotDormant);
        else
            kernel().finish(ctx, as_unsigned(thread->exit_status));
    });

    // The *CB variants run the thread's notified callbacks before waiting.
    const auto sleep_cb = [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().current_thread();
        (void)kernel().deliver_callbacks();
        if (thread != nullptr && thread->wakeup_count != 0u) {
            --thread->wakeup_count;
            kernel().finish(ctx, 0u);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::Sleep;
        kernel().block(ctx, wait, 0u);
    };
    const auto sleep = [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().current_thread();
        if (thread != nullptr && thread->wakeup_count != 0u) {
            --thread->wakeup_count;
            kernel().finish(ctx, 0u);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::Sleep;
        kernel().block(ctx, wait, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelSleepThread", sleep);
    hle.add("ThreadManForUser", "sceKernelSleepThreadCB", sleep_cb);
    hle.add("ThreadManForUser", "sceKernelWakeupThread", [](Runtime &, AllegrexContext &ctx) {
        Thread *thread = kernel().find_thread(as_signed(arg(ctx, 0)));
        if (thread == nullptr) {
            kernel().finish(ctx, error::kUnknownThid);
            return;
        }
        if (trace_sync()) log_sync("WakeupThread " + std::to_string(thread->uid) + " " + thread->name);
        if (thread->status == ThreadStatus::Waiting && thread->wait.type == WaitType::Sleep)
            kernel().wake(*thread, 0u);
        else
            ++thread->wakeup_count;
        kernel().finish(ctx, 0u);
    });

    const auto delay = [](Runtime &, AllegrexContext &ctx) { kernel().delay_current(ctx, arg(ctx, 0)); };
    hle.add("ThreadManForUser", "sceKernelDelayThread", delay);
    hle.add("ThreadManForUser", "sceKernelDelayThreadCB", [](Runtime &, AllegrexContext &ctx) {
        (void)kernel().deliver_callbacks();
        kernel().delay_current(ctx, arg(ctx, 0));
    });

    hle.add("ThreadManForUser", "sceKernelSuspendDispatchThread", [](Runtime &, AllegrexContext &ctx) {
        const bool was_enabled = kernel().dispatch_enabled();
        kernel().set_dispatch_enabled(false);
        kernel().finish(ctx, was_enabled ? 1u : 0u);
    });
    hle.add("ThreadManForUser", "sceKernelResumeDispatchThread", [](Runtime &, AllegrexContext &ctx) {
        kernel().set_dispatch_enabled(arg(ctx, 0) != 0u);
        kernel().finish(ctx, 0u);
    });
}

void register_time(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelGetSystemTime", [](Runtime &rt, AllegrexContext &ctx) {
        store64(rt.memory(), arg(ctx, 0), kernel().now_us());
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelGetSystemTimeWide",
        [](Runtime &, AllegrexContext &ctx) { kernel().finish64(ctx, kernel().now_us()); });
    hle.add("ThreadManForUser", "sceKernelGetSystemTimeLow",
        [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, static_cast<std::uint32_t>(kernel().now_us())); });
    hle.add("ThreadManForUser", "sceKernelSysClock2USecWide", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint64_t clock = arg64(ctx, 0);
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(clock / 1'000'000u));
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), static_cast<std::uint32_t>(clock % 1'000'000u));
        kernel().finish(ctx, 0u);
    });
}

void register_semaphores(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateSema", [](Runtime &rt, AllegrexContext &ctx) {
        const auto initial = as_signed(arg(ctx, 2));
        const auto maximum = as_signed(arg(ctx, 3));
        if (maximum <= 0 || initial < 0 || initial > maximum) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        const SceUID uid = kernel().allocate_uid();
        kernel().semaphores[uid] =
            Semaphore{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), initial, maximum, {}};
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteSema", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().semaphores.find(as_signed(arg(ctx, 0)));
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().semaphores.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSignalSema", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().semaphores.find(uid);
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        const auto signal = as_signed(arg(ctx, 1));
        if (found->second.count + signal - static_cast<std::int32_t>(found->second.waiters.size()) >
            found->second.max_count) {
            kernel().finish(ctx, error::kSemaOverflow);
            return;
        }
        found->second.count += signal;
        kernel().release_semaphore_waiters(uid);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelWaitSema", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().semaphores.find(uid);
        const auto wanted = as_signed(arg(ctx, 1));
        if (found == kernel().semaphores.end()) {
            kernel().finish(ctx, error::kUnknownSemid);
            return;
        }
        if (wanted <= 0 || wanted > found->second.max_count) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        Semaphore &sema = found->second;
        if (sema.waiters.empty() && sema.count >= wanted) {
            sema.count -= wanted;
            kernel().finish(ctx, 0u);
            return;
        }
        sema.waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::Semaphore;
        wait.object = uid;
        wait.value = static_cast<std::uint32_t>(wanted);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelPollSema", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().semaphores.find(as_signed(arg(ctx, 0)));
        const auto wanted = as_signed(arg(ctx, 1));
        if (found == kernel().semaphores.end())
            kernel().finish(ctx, error::kUnknownSemid);
        else if (wanted <= 0)
            kernel().finish(ctx, error::kIllegalCount);
        else if (found->second.waiters.empty() && found->second.count >= wanted) {
            found->second.count -= wanted;
            kernel().finish(ctx, 0u);
        } else {
            kernel().finish(ctx, error::kSemaZero);
        }
    });
}

void register_event_flags(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateEventFlag", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        kernel().event_flags[uid] =
            EventFlag{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), arg(ctx, 2), {}};
        if (trace_sync()) log_sync("CreateEventFlag " + std::to_string(uid) + " " + kernel().event_flags[uid].name);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteEventFlag", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().event_flags.find(as_signed(arg(ctx, 0)));
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().event_flags.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSetEventFlag", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        auto found = kernel().event_flags.find(uid);
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        found->second.pattern |= arg(ctx, 1);
        if (trace_sync())
            log_sync("SetEventFlag " + std::to_string(uid) + " " + found->second.name +
                " bits=" + psprecomp::hex32(arg(ctx, 1)) + " -> " + psprecomp::hex32(found->second.pattern));
        kernel().release_event_flag_waiters(uid);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelClearEventFlag", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().event_flags.find(as_signed(arg(ctx, 0)));
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return;
        }
        found->second.pattern &= arg(ctx, 1);
        kernel().finish(ctx, 0u);
    });

    // Shared validation and immediate-match logic for Wait/Poll. Returns true
    // when the call was completed without blocking.
    const auto try_match = [](Runtime &rt, AllegrexContext &ctx, bool poll) -> bool {
        const SceUID uid = as_signed(arg(ctx, 0));
        const std::uint32_t bits = arg(ctx, 1);
        const std::uint32_t mode = arg(ctx, 2);
        const std::uint32_t out = arg(ctx, 3);
        auto found = kernel().event_flags.find(uid);
        if (found == kernel().event_flags.end()) {
            kernel().finish(ctx, error::kUnknownEvfid);
            return true;
        }
        if ((mode & ~0x31u) != 0u || (mode & 0x30u) == 0x30u) {
            kernel().finish(ctx, error::kIllegalMode);
            return true;
        }
        if (bits == 0u) {
            kernel().finish(ctx, error::kEvfIllegalPattern);
            return true;
        }
        EventFlag &flag = found->second;
        if (Kernel::event_flag_matches(flag.pattern, bits, mode)) {
            if (out != 0u) rt.memory().store32(out, flag.pattern);
            if ((mode & 0x10u) != 0u)
                flag.pattern = 0u;
            else if ((mode & 0x20u) != 0u)
                flag.pattern &= ~bits;
            kernel().finish(ctx, 0u);
            return true;
        }
        if (poll) {
            if (out != 0u) rt.memory().store32(out, flag.pattern);
            kernel().finish(ctx, error::kEvfCond);
            return true;
        }
        if ((flag.attributes & kEventFlagWaitMulti) == 0u && !flag.waiters.empty()) {
            kernel().finish(ctx, error::kEvfMulti);
            return true;
        }
        return false;
    };
    hle.add("ThreadManForUser", "sceKernelPollEventFlag",
        [try_match](Runtime &rt, AllegrexContext &ctx) { (void)try_match(rt, ctx, true); });
    hle.add("ThreadManForUser", "sceKernelWaitEventFlag", [try_match](Runtime &rt, AllegrexContext &ctx) {
        if (try_match(rt, ctx, false)) return;
        const SceUID uid = as_signed(arg(ctx, 0));
        if (trace_sync())
            log_sync("WaitEventFlag " + std::to_string(uid) + " " + kernel().event_flags[uid].name + " bits=" +
                psprecomp::hex32(arg(ctx, 1)) + " pattern=" + psprecomp::hex32(kernel().event_flags[uid].pattern));
        kernel().event_flags[uid].waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::EventFlag;
        wait.object = uid;
        wait.value = arg(ctx, 1);
        wait.mode = arg(ctx, 2);
        wait.out_address = arg(ctx, 3);
        wait.timeout_address = arg(ctx, 4);
        kernel().block(ctx, wait, 0u);
    });
}

void register_mutexes(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateMutex", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        Mutex mutex{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), 0, as_signed(arg(ctx, 2)), {}};
        if (mutex.lock_count > 0) mutex.owner = kernel().current_uid();
        kernel().mutexes[uid] = std::move(mutex);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteMutex", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().mutexes.find(as_signed(arg(ctx, 0)));
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().mutexes.erase(found);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelLockMutex", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto count = as_signed(arg(ctx, 1));
        auto found = kernel().mutexes.find(uid);
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        Mutex &mutex = found->second;
        if (mutex.lock_count == 0) {
            mutex.owner = kernel().current_uid();
            mutex.lock_count = count;
            kernel().finish(ctx, 0u);
            return;
        }
        if (mutex.owner == kernel().current_uid()) {
            if ((mutex.attributes & kMutexAttrRecursive) == 0u) {
                kernel().finish(ctx, error::kMutexRecursiveNotAllowed);
                return;
            }
            mutex.lock_count += count;
            kernel().finish(ctx, 0u);
            return;
        }
        mutex.waiters.push_back(kernel().current_uid());
        WaitState wait{};
        wait.type = WaitType::Mutex;
        wait.object = uid;
        wait.value = static_cast<std::uint32_t>(count);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelUnlockMutex", [](Runtime &, AllegrexContext &ctx) {
        const SceUID uid = as_signed(arg(ctx, 0));
        const auto count = as_signed(arg(ctx, 1));
        auto found = kernel().mutexes.find(uid);
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        Mutex &mutex = found->second;
        if (count <= 0)
            kernel().finish(ctx, error::kIllegalCount);
        else if (mutex.lock_count == 0 || mutex.owner != kernel().current_uid())
            kernel().finish(ctx, error::kMutexUnlocked);
        else if (count > mutex.lock_count)
            kernel().finish(ctx, error::kMutexUnlockUnderflow);
        else {
            mutex.lock_count -= count;
            if (mutex.lock_count == 0) {
                mutex.owner = 0;
                kernel().release_mutex_waiters(uid);
            }
            kernel().finish(ctx, 0u);
        }
    });
}

void register_callbacks_and_timers(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateCallback", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        kernel().callbacks[uid] =
            Callback{read_cstring(rt.memory(), arg(ctx, 0), 32u), arg(ctx, 1), arg(ctx, 2), kernel().current_uid()};
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteCallback", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, kernel().callbacks.erase(as_signed(arg(ctx, 0))) != 0u ? 0u : error::kUnknownCbid);
    });

    hle.add("ThreadManForUser", "sceKernelCreateVTimer", [](Runtime &rt, AllegrexContext &ctx) {
        const SceUID uid = kernel().allocate_uid();
        VTimer timer{};
        timer.name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        kernel().vtimers[uid] = timer;
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelStartVTimer", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().vtimers.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vtimers.end()) {
            kernel().finish(ctx, error::kUnknownVtid);
            return;
        }
        if (found->second.active) {
            kernel().finish(ctx, 1u);
            return;
        }
        found->second.active = true;
        found->second.base_us = kernel().now_us();
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelSetVTimerHandlerWide", [](Runtime &, AllegrexContext &ctx) {
        auto found = kernel().vtimers.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vtimers.end()) {
            kernel().finish(ctx, error::kUnknownVtid);
            return;
        }
        found->second.schedule_us = arg64(ctx, 2);
        found->second.handler = arg(ctx, 4);
        found->second.common = arg(ctx, 5);
        kernel().finish(ctx, 0u);
    });
}

void register_kernel_library(HleRegistrar &hle) {
    hle.add("Kernel_Library", "sceKernelCpuSuspendIntr", [](Runtime &, AllegrexContext &ctx) {
        const bool was_enabled = kernel().interrupts_enabled();
        kernel().set_interrupts_enabled(false);
        kernel().finish(ctx, was_enabled ? 1u : 0u);
    });
    hle.add("Kernel_Library", "sceKernelCpuResumeIntr", [](Runtime &, AllegrexContext &ctx) {
        kernel().set_interrupts_enabled(arg(ctx, 0) != 0u);
        kernel().finish(ctx, 0u);
    });
    hle.add("Kernel_Library", "sceKernelMemset", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 0);
        const auto value = static_cast<std::uint8_t>(arg(ctx, 1));
        const std::uint32_t size = arg(ctx, 2);
        for (std::uint32_t i = 0; i < size; ++i) rt.memory().store8(address + i, value);
        kernel().finish(ctx, address);
    });
}

constexpr std::uint32_t kVplAlignment = 0x40u;
constexpr std::uint32_t kVplInfoSize = 52u;

std::optional<std::uint32_t> vpl_candidate(Vpl &vpl, std::uint32_t size) {
    if (size == 0u || size > vpl.pool_size) return std::nullopt;
    const auto try_gap = [size](std::uint32_t start, std::uint32_t end) -> std::optional<std::uint32_t> {
        const std::uint32_t aligned = (start + kVplAlignment - 1u) & ~(kVplAlignment - 1u);
        if (static_cast<std::uint64_t>(aligned) + size <= end) return aligned;
        return std::nullopt;
    };
    std::uint32_t start = vpl.pool_address;
    for (const auto &[address, allocated] : vpl.allocations) {
        if (const auto candidate = try_gap(start, address)) return candidate;
        start = std::max(start, address + allocated);
    }
    return try_gap(start, vpl.pool_address + vpl.pool_size);
}

std::uint32_t vpl_free_size(const Vpl &vpl) {
    std::uint32_t used = 0u;
    for (const auto &[address, size] : vpl.allocations) {
        (void)address;
        used += size;
    }
    return vpl.pool_size - used;
}

void register_vpl(HleRegistrar &hle) {
    hle.add("ThreadManForUser", "sceKernelCreateVpl", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string name = read_cstring(rt.memory(), arg(ctx, 0), 32u);
        const std::uint32_t attributes = arg(ctx, 2);
        std::uint32_t size = arg(ctx, 3);
        if (size == 0u || size >= 0x80000000u) {
            kernel().finish(ctx, error::kIllegalMemsize);
            return;
        }
        if (size <= 0x30u) size = 0x1000u;
        size = (size + 7u) & ~7u;
        const std::int32_t block = kernel().allocate_block("VPL/" + name,
            (attributes & 0x00010000u) != 0u ? 1u : 0u, size, 0u);
        if (block < 0) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        const MemoryBlock *pool = kernel().find_block(block);
        Vpl vpl{};
        vpl.name = name;
        vpl.attributes = attributes;
        vpl.pool_address = pool->address;
        vpl.pool_size = pool->size;
        vpl.pool_block = block;
        const SceUID uid = kernel().allocate_uid();
        kernel().vpls[uid] = std::move(vpl);
        kernel().finish(ctx, as_unsigned(uid));
    });
    hle.add("ThreadManForUser", "sceKernelDeleteVpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = kernel().vpls.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vpls.end()) {
            kernel().finish(ctx, error::kUnknownVplid);
            return;
        }
        (void)kernel().free_block(found->second.pool_block);
        kernel().vpls.erase(found);
        kernel().finish(ctx, 0u);
    });
    const auto try_allocate = [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = kernel().vpls.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vpls.end()) {
            kernel().finish(ctx, error::kUnknownVplid);
            return;
        }
        const std::uint32_t size = arg(ctx, 1);
        if (size == 0u || size > found->second.pool_size) {
            kernel().finish(ctx, error::kIllegalMemsize);
            return;
        }
        const auto address = vpl_candidate(found->second, size);
        if (!address) {
            kernel().finish(ctx, error::kNoMemory);
            return;
        }
        found->second.allocations[*address] = size;
        rt.memory().store32(arg(ctx, 2), *address);
        kernel().finish(ctx, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelTryAllocateVpl", try_allocate);
    hle.add("ThreadManForUser", "sceKernelAllocateVpl", [try_allocate](Runtime &rt, AllegrexContext &ctx) {
        // No blocking path yet: a full pool reports no memory instead of
        // parking the caller. Boot traffic uses the try form.
        try_allocate(rt, ctx);
    });
    hle.add("ThreadManForUser", "sceKernelFreeVpl", [](Runtime &, AllegrexContext &ctx) {
        const auto found = kernel().vpls.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vpls.end()) {
            kernel().finish(ctx, error::kUnknownVplid);
            return;
        }
        const auto allocation = found->second.allocations.find(arg(ctx, 1));
        if (allocation == found->second.allocations.end()) {
            kernel().finish(ctx, error::kIllegalMemblock);
            return;
        }
        found->second.allocations.erase(allocation);
        kernel().finish(ctx, 0u);
    });
    hle.add("ThreadManForUser", "sceKernelReferVplStatus", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = kernel().vpls.find(as_signed(arg(ctx, 0)));
        if (found == kernel().vpls.end()) {
            kernel().finish(ctx, error::kUnknownVplid);
            return;
        }
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 1);
        memory.store32(out, kVplInfoSize);
        write_cstring(memory, out + 4u, found->second.name, 32u);
        memory.store32(out + 36u, found->second.attributes);
        memory.store32(out + 40u, found->second.pool_size);
        memory.store32(out + 44u, vpl_free_size(found->second));
        memory.store32(out + 48u, 0u);
        kernel().finish(ctx, 0u);
    });
}

constexpr std::uint32_t kLwMutexWorkSize = 32u;

void mirror_lwmutex(psprecomp::GuestMemory &memory, std::uint32_t workarea, const Mutex &mutex) {
    if (workarea == 0u) return;
    memory.store32(workarea, static_cast<std::uint32_t>(mutex.lock_count));
    memory.store32(workarea + 4u, static_cast<std::uint32_t>(mutex.owner));
    memory.store32(workarea + 12u, static_cast<std::uint32_t>(mutex.waiters.size()));
}

std::int32_t lwmutex_uid(psprecomp::GuestMemory &memory, std::uint32_t workarea) {
    return static_cast<std::int32_t>(memory.load32(workarea + 16u));
}

void register_lwmutex(HleRegistrar &hle) {
    const auto create = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t workarea = arg(ctx, 0);
        const std::string name = read_cstring(rt.memory(), arg(ctx, 1), 32u);
        const std::uint32_t attributes = arg(ctx, 2);
        const auto initial = static_cast<std::int32_t>(arg(ctx, 3));
        if (workarea == 0u || (workarea & 3u) != 0u) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        if (attributes >= 0x400u) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        if (initial < 0 || (initial > 1 && (attributes & kMutexAttrRecursive) == 0u)) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        auto &memory = rt.memory();
        for (std::uint32_t offset = 0; offset < kLwMutexWorkSize; offset += 4u) memory.store32(workarea + offset, 0u);
        const SceUID uid = kernel().allocate_uid();
        Mutex mutex{};
        mutex.name = name;
        mutex.attributes = attributes;
        mutex.owner = initial > 0 ? kernel().current_uid() : 0;
        mutex.lock_count = initial;
        mutex.workarea = workarea;
        kernel().mutexes[uid] = mutex;
        mirror_lwmutex(memory, workarea, mutex);
        memory.store32(workarea + 8u, attributes);
        memory.store32(workarea + 16u, static_cast<std::uint32_t>(uid));
        kernel().finish(ctx, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelCreateLwMutex", create);

    hle.add("ThreadManForUser", "sceKernelDeleteLwMutex", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t workarea = arg(ctx, 0);
        if (workarea == 0u) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        const auto found = kernel().mutexes.find(lwmutex_uid(rt.memory(), workarea));
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        kernel().cancel_waiters(found->second.waiters, error::kWaitDelete);
        kernel().mutexes.erase(found);
        auto &memory = rt.memory();
        memory.store32(workarea, 0u);
        memory.store32(workarea + 4u, 0xFFFFFFFFu);
        memory.store32(workarea + 16u, 0xFFFFFFFFu);
        kernel().finish(ctx, 0u);
    });

    const auto try_lock = [](Runtime &rt, AllegrexContext &ctx, bool blocking) {
        const std::uint32_t workarea = arg(ctx, 0);
        const auto count = static_cast<std::int32_t>(arg(ctx, 1));
        const auto found = kernel().mutexes.find(lwmutex_uid(rt.memory(), workarea));
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        Mutex &mutex = found->second;
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
            return;
        }
        if (mutex.owner == kernel().current_uid()) {
            if ((mutex.attributes & kMutexAttrRecursive) == 0u) {
                kernel().finish(ctx, error::kMutexLocked);
                return;
            }
            mutex.lock_count += count;
            mirror_lwmutex(rt.memory(), workarea, mutex);
            kernel().finish(ctx, 0u);
            return;
        }
        if (mutex.lock_count == 0) {
            mutex.owner = kernel().current_uid();
            mutex.lock_count = count;
            mirror_lwmutex(rt.memory(), workarea, mutex);
            kernel().finish(ctx, 0u);
            return;
        }
        if (!blocking) {
            kernel().finish(ctx, error::kMutexLocked);
            return;
        }
        WaitState wait{};
        wait.type = WaitType::Mutex;
        wait.object = found->first;
        wait.value = static_cast<std::uint32_t>(count);
        wait.timeout_address = arg(ctx, 2);
        kernel().block(ctx, wait, 0u);
    };
    hle.add("ThreadManForUser", "sceKernelLockLwMutex", [try_lock](Runtime &rt, AllegrexContext &ctx) {
        try_lock(rt, ctx, true);
    });
    hle.add("Kernel_Library", "sceKernelLockLwMutex", [try_lock](Runtime &rt, AllegrexContext &ctx) {
        try_lock(rt, ctx, true);
    });
    hle.add("Kernel_Library", "sceKernelTryLockLwMutex", [try_lock](Runtime &rt, AllegrexContext &ctx) {
        try_lock(rt, ctx, false);
    });

    const auto unlock = [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t workarea = arg(ctx, 0);
        const auto count = static_cast<std::int32_t>(arg(ctx, 1));
        const auto found = kernel().mutexes.find(lwmutex_uid(rt.memory(), workarea));
        if (found == kernel().mutexes.end()) {
            kernel().finish(ctx, error::kMutexNotFound);
            return;
        }
        Mutex &mutex = found->second;
        if (count <= 0) {
            kernel().finish(ctx, error::kIllegalCount);
        } else if (mutex.lock_count == 0 || mutex.owner != kernel().current_uid()) {
            kernel().finish(ctx, error::kMutexUnlocked);
        } else if (count > mutex.lock_count) {
            kernel().finish(ctx, error::kMutexUnlockUnderflow);
        } else {
            mutex.lock_count -= count;
            if (mutex.lock_count == 0) {
                mutex.owner = 0;
                kernel().release_mutex_waiters(found->first);
            }
            mirror_lwmutex(rt.memory(), workarea, mutex);
            kernel().finish(ctx, 0u);
        }
    };
    hle.add("ThreadManForUser", "sceKernelUnlockLwMutex", unlock);
    hle.add("Kernel_Library", "sceKernelUnlockLwMutex", unlock);
}

} // namespace

void register_threadman(HleRegistrar &hle) {
    register_threads(hle);
    register_time(hle);
    register_semaphores(hle);
    register_event_flags(hle);
    register_mutexes(hle);
    register_callbacks_and_timers(hle);
    register_vpl(hle);
    register_lwmutex(hle);
    register_kernel_library(hle);
}

} // namespace gungnir::hle

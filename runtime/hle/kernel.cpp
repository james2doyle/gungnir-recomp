#include "kernel.hpp"

#include "session/snapshot.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kMemoryGranularity = 0x100u;
constexpr std::uint32_t kLoaderPriority = 0x20u;
constexpr std::uint32_t kLoaderStackSize = 0x40000u;
constexpr std::uint32_t kThreadAttrNoFillStack = 0x00100000u;
constexpr std::uint32_t kThreadArgumentHome = 0x40u;
// Virtual time charged per runtime starvation boundary, i.e. per
// kStarvationInterval dispatches without an import call.
constexpr std::uint64_t kStarvationQuantumUs = 1000u;
// Idle vblanks with no runnable thread before the scheduler reports a deadlock.
constexpr std::uint64_t kIdleVBlankLimit = 60u * 60u * 5u;
constexpr std::int32_t kInterruptIdentity = -2;
constexpr std::int32_t kIdleIdentity = -3;
// Scratch for SceKernelSysClock values passed to VTimer handlers.
constexpr std::uint32_t kVTimerClockScratch = 0x08180000u;

// PSP reserves 256 bytes at the top of every thread stack for the thread
// control block and points $k0 at it; SDK code reads and writes it directly.
constexpr std::uint32_t kThreadControlBlockSize = 0x100u;

std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool trace_enabled() {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_KERNEL") != nullptr;
    return enabled;
}

void native_thread_exit(Runtime &, AllegrexContext &ctx) {
    kernel().thread_exit_stub(ctx);
}
void native_interrupt_return(Runtime &, AllegrexContext &ctx) {
    kernel().interrupt_return_stub(ctx);
}
void native_idle(Runtime &, AllegrexContext &ctx) {
    kernel().idle_stub(ctx);
}
void native_guest_call_return(Runtime &, AllegrexContext &ctx) {
    kernel().guest_call_return_stub(ctx);
}
void native_starvation(Runtime &, AllegrexContext &ctx) {
    kernel().on_starvation(ctx);
}

const char *status_name(ThreadStatus status) {
    switch (status) {
    case ThreadStatus::Dormant:
        return "dormant";
    case ThreadStatus::Ready:
        return "ready";
    case ThreadStatus::Running:
        return "running";
    case ThreadStatus::Waiting:
        return "waiting";
    case ThreadStatus::Dead:
        return "dead";
    }
    return "?";
}

const char *wait_name(WaitType type) {
    switch (type) {
    case WaitType::None:
        return "none";
    case WaitType::Delay:
        return "delay";
    case WaitType::Sleep:
        return "sleep";
    case WaitType::Semaphore:
        return "sema";
    case WaitType::EventFlag:
        return "eventflag";
    case WaitType::Mutex:
        return "mutex";
    case WaitType::VBlank:
        return "vblank";
    case WaitType::ThreadEnd:
        return "thread-end";
    case WaitType::Fpl:
        return "fpl";
    case WaitType::MsgPipeSend:
        return "msgpipe-send";
    case WaitType::MsgPipeReceive:
        return "msgpipe-receive";
    case WaitType::Audio:
        return "audio";
    case WaitType::Host:
        return "host";
    }
    return "?";
}

void copy_guest_to_guest(psprecomp::GuestMemory &memory, std::uint32_t destination,
    std::uint32_t source, std::uint32_t size) {
    // Overlap is possible when a guest sends and receives through the same
    // buffer; copy through a temporary in that case.
    if (destination + size <= source || source + size <= destination) {
        for (std::uint32_t i = 0u; i < size; ++i) memory.store8(destination + i, memory.load8(source + i));
        return;
    }
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t i = 0u; i < size; ++i) bytes[i] = memory.load8(source + i);
    for (std::uint32_t i = 0u; i < size; ++i) memory.store8(destination + i, bytes[i]);
}

int take_fpl_block(Fpl &fpl) noexcept {
    for (std::uint32_t index = 0u; index < fpl.used.size(); ++index) {
        if (!fpl.used[index]) {
            fpl.used[index] = true;
            return static_cast<int>(index);
        }
    }
    return -1;
}

} // namespace

Kernel &kernel() {
    static Kernel instance;
    return instance;
}

void Kernel::install(Runtime &runtime, std::uint32_t gp, std::uint32_t image_end) {
    runtime_ = &runtime;
    gp_ = gp;
    pristine_context_ = AllegrexContext{};
    pristine_context_.eat_vfpu_prefixes();
    pristine_context_.set_gpr(28, gp);

    free_ranges_.clear();
    const std::uint32_t user_start = align_up(image_end, 0x1000u);
    free_ranges_.push_back(FreeRange{user_start, kUserMemoryEnd - user_start});

    runtime.register_function(kThreadExitStub, &native_thread_exit, "mhp3rd_thread_exit");
    runtime.register_function(kInterruptReturnStub, &native_interrupt_return, "mhp3rd_interrupt_return");
    runtime.register_function(kIdleStub, &native_idle, "mhp3rd_idle");
    runtime.register_function(kGuestCallReturnStub, &native_guest_call_return, "mhp3rd_guest_call_return");
    std::uint64_t interval = 20'000u;
    if (const char *text = std::getenv("GUNGNIR_STARVATION_INTERVAL")) interval = std::strtoull(text, nullptr, 0);
    psprecomp::set_runtime_starvation_hook(&native_starvation, interval);
}

// ---------------------------------------------------------------------------
// Threads

// Fills the stack, lays out the $k0 control block and returns the initial $sp.
std::uint32_t Kernel::prepare_thread_stack(Thread &thread) {
    auto &memory = runtime_->memory();
    if ((thread.attributes & kThreadAttrNoFillStack) == 0u) {
        for (std::uint32_t offset = 0; offset < thread.stack_size; offset += 4u)
            memory.store32(thread.stack_bottom + offset, 0xFFFFFFFFu);
    }
    const std::uint32_t control_block = thread.stack_bottom + thread.stack_size - kThreadControlBlockSize;
    for (std::uint32_t offset = 0; offset < kThreadControlBlockSize; offset += 4u)
        memory.store32(control_block + offset, 0u);
    memory.store32(control_block + 0xC0u, static_cast<std::uint32_t>(thread.uid));
    memory.store32(control_block + 0xC8u, thread.stack_bottom);
    memory.store32(control_block + 0xF8u, 0xFFFFFFFFu);
    memory.store32(control_block + 0xFCu, 0xFFFFFFFFu);
    memory.store32(thread.stack_bottom, static_cast<std::uint32_t>(thread.uid));
    thread.control_block = control_block;
    return control_block;
}

Thread *Kernel::find_thread(SceUID uid) noexcept {
    const auto found = threads_.find(uid);
    return found != threads_.end() ? found->second.get() : nullptr;
}

void Kernel::start_loader_thread(AllegrexContext &ctx, std::uint32_t entry, std::uint32_t stack_top) {
    auto thread = std::make_unique<Thread>();
    thread->uid = allocate_uid();
    thread->name = "module_start";
    thread->entry = entry;
    thread->priority = thread->initial_priority = kLoaderPriority;
    thread->gp = gp_;
    thread->stack_size = kLoaderStackSize;
    thread->stack_block = allocate_block("module_start stack", 1u, kLoaderStackSize, 0u);
    if (const MemoryBlock *block = find_block(thread->stack_block)) {
        thread->stack_bottom = block->address;
        stack_top = block->address + block->size;
    }
    thread->status = ThreadStatus::Running;
    (void)stack_top;
    const std::uint32_t sp = prepare_thread_stack(*thread);

    const std::uint32_t a0 = ctx.gpr[4];
    const std::uint32_t a1 = ctx.gpr[5];
    ctx = pristine_context_;
    ctx.set_gpr(4, a0);
    ctx.set_gpr(5, a1);
    ctx.set_gpr(26, thread->control_block);
    ctx.set_gpr(29, sp - kThreadArgumentHome);
    ctx.set_gpr(31, kThreadExitStub);
    ctx.pc = entry;

    current_uid_ = thread->uid;
    psprecomp::set_runtime_thread_identity(thread->uid, thread->name);
    threads_.emplace(thread->uid, std::move(thread));
}

std::int32_t Kernel::create_thread(const std::string &name, std::uint32_t entry, std::uint32_t priority,
    std::uint32_t stack_size, std::uint32_t attributes, std::uint32_t gp) {
    if (entry == 0u || (entry & 3u) != 0u) return static_cast<std::int32_t>(error::kIllegalEntry);
    if (priority == 0u || priority > 0x7Fu) return static_cast<std::int32_t>(error::kIllegalPriority);
    if (stack_size < 0x200u) return static_cast<std::int32_t>(error::kIllegalStackSize);

    auto thread = std::make_unique<Thread>();
    thread->uid = allocate_uid();
    thread->name = name;
    thread->entry = entry;
    thread->priority = thread->initial_priority = priority;
    thread->attributes = attributes;
    thread->gp = gp;
    thread->stack_size = align_up(stack_size, kMemoryGranularity);
    const std::int32_t block = allocate_block("stack:" + name, 1u, thread->stack_size, 0u);
    if (block < 0) return static_cast<std::int32_t>(error::kNoMemory);
    thread->stack_block = block;
    thread->stack_bottom = find_block(block)->address;
    (void)prepare_thread_stack(*thread);
    thread->status = ThreadStatus::Dormant;
    const SceUID uid = thread->uid;
    threads_.emplace(uid, std::move(thread));
    if (trace_enabled())
        std::cerr << "[kernel] create thread uid=" << uid << " name=" << name << " entry=" << psprecomp::hex32(entry)
                  << " prio=" << psprecomp::hex32(priority) << "\n";
    return uid;
}

std::int32_t Kernel::start_thread(
    AllegrexContext &ctx, SceUID uid, std::uint32_t argument_size, std::uint32_t argument_address) {
    Thread *thread = find_thread(uid);
    if (thread == nullptr) return static_cast<std::int32_t>(error::kUnknownThid);
    if (thread->status != ThreadStatus::Dormant) return static_cast<std::int32_t>(error::kNotDormant);
    (void)ctx;

    auto &memory = runtime_->memory();
    AllegrexContext &context = thread->context;
    context = pristine_context_;
    context.set_gpr(28, thread->gp);
    std::uint32_t sp = prepare_thread_stack(*thread);
    std::uint32_t argument_copy = 0u;
    if (argument_address != 0u && argument_size != 0u) {
        sp -= align_up(argument_size, 0x10u);
        for (std::uint32_t i = 0; i < argument_size; ++i) memory.store8(sp + i, memory.load8(argument_address + i));
        argument_copy = sp;
    }
    context.set_gpr(4, argument_size);
    context.set_gpr(5, argument_copy);
    context.set_gpr(26, thread->control_block);
    context.set_gpr(29, sp - kThreadArgumentHome);
    context.set_gpr(31, kThreadExitStub);
    context.pc = thread->entry;
    thread->priority = thread->initial_priority;
    thread->wakeup_count = 0u;
    thread->exit_status = 0;
    make_ready(*thread);
    if (trace_enabled()) std::cerr << "[kernel] start thread uid=" << uid << " name=" << thread->name << "\n";
    return 0;
}

void Kernel::exit_current_thread(AllegrexContext &ctx, std::int32_t status, bool delete_thread) {
    Thread *thread = current_thread();
    if (trace_enabled())
        std::cerr << "[kernel] exit thread uid=" << current_uid_ << " name=" << (thread != nullptr ? thread->name : "?")
                  << " status=" << status << "\n";
    if (thread != nullptr) {
        thread->exit_status = status;
        thread->status = ThreadStatus::Dormant;
        const SceUID uid = thread->uid;
        wake_thread_end_waiters(uid, status);
        if (delete_thread) (void)this->delete_thread(uid);
    }
    current_uid_ = 0;
    schedule(ctx);
}

std::int32_t Kernel::terminate_thread(AllegrexContext &ctx, SceUID uid, bool delete_thread) {
    (void)ctx;
    Thread *thread = find_thread(uid);
    if (thread == nullptr) return static_cast<std::int32_t>(error::kUnknownThid);
    if (uid == current_uid_) return static_cast<std::int32_t>(error::kIllegalThid);
    if (thread->status == ThreadStatus::Waiting) remove_waiter(*thread);
    thread->status = ThreadStatus::Dormant;
    thread->exit_status = static_cast<std::int32_t>(0x800201ACu);
    wake_thread_end_waiters(uid, thread->exit_status);
    if (delete_thread) return this->delete_thread(uid);
    return 0;
}

std::int32_t Kernel::delete_thread(SceUID uid) {
    Thread *thread = find_thread(uid);
    if (thread == nullptr) return static_cast<std::int32_t>(error::kUnknownThid);
    if (thread->status != ThreadStatus::Dormant && thread->status != ThreadStatus::Dead)
        return static_cast<std::int32_t>(error::kNotDormant);
    if (thread->stack_block != 0) (void)free_block(thread->stack_block);
    threads_.erase(uid);
    return 0;
}

std::int32_t Kernel::change_priority(AllegrexContext &ctx, SceUID uid, std::uint32_t priority) {
    (void)ctx;
    Thread *thread = uid == 0 ? current_thread() : find_thread(uid);
    if (thread == nullptr) return static_cast<std::int32_t>(error::kUnknownThid);
    if (thread->status == ThreadStatus::Dormant) return static_cast<std::int32_t>(error::kDormant);
    if (priority == 0u) priority = current_thread() != nullptr ? current_thread()->priority : thread->priority;
    if (priority > 0x7Fu) return static_cast<std::int32_t>(error::kIllegalPriority);
    thread->priority = priority;
    if (thread->status == ThreadStatus::Ready) thread->ready_sequence = next_ready_sequence_++;
    return 0;
}

// ---------------------------------------------------------------------------
// Scheduling

void Kernel::make_ready(Thread &thread) {
    thread.status = ThreadStatus::Ready;
    thread.wait = WaitState{};
    thread.ready_sequence = next_ready_sequence_++;
}

Thread *Kernel::best_ready_thread() noexcept {
    Thread *best = nullptr;
    for (auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread->status != ThreadStatus::Ready) continue;
        if (best == nullptr || thread->priority < best->priority ||
            (thread->priority == best->priority && thread->ready_sequence < best->ready_sequence))
            best = thread.get();
    }
    return best;
}

void Kernel::save_current(const AllegrexContext &ctx, ThreadStatus status) {
    Thread *thread = current_thread();
    if (thread == nullptr) return;
    thread->context = ctx;
    if (status == ThreadStatus::Ready)
        make_ready(*thread);
    else
        thread->status = status;
}

void Kernel::switch_to(AllegrexContext &ctx, Thread &thread) {
    thread.status = ThreadStatus::Running;
    current_uid_ = thread.uid;
    ctx = thread.context;
    psprecomp::set_runtime_thread_identity(thread.uid, thread.name);
    idle_vblanks_ = 0u;
}

void Kernel::finish(AllegrexContext &ctx, std::uint32_t result) {
    ctx.set_gpr(2, result);
    if (interrupt_active_ || current_thread() == nullptr) return;

    if (interrupts_enabled_ && !pending_interrupts_.empty()) {
        interrupted_context_ = ctx;
        interrupted_context_.pc = ctx.gpr[31];
        interrupted_idle_ = false;
        (void)begin_pending_interrupt(ctx);
        return;
    }
    if (!dispatch_enabled_) return;
    Thread *best = best_ready_thread();
    Thread *current = current_thread();
    if (best != nullptr && best->priority < current->priority) {
        AllegrexContext saved = ctx;
        saved.pc = ctx.gpr[31];
        save_current(saved, ThreadStatus::Ready);
        switch_to(ctx, *best);
    }
}

void Kernel::finish64(AllegrexContext &ctx, std::uint64_t result) {
    ctx.set_gpr(3, static_cast<std::uint32_t>(result >> 32u));
    finish(ctx, static_cast<std::uint32_t>(result));
}

void Kernel::block(AllegrexContext &ctx, const WaitState &wait, std::uint32_t result) {
    Thread *thread = current_thread();
    if (interrupt_active_ || thread == nullptr) {
        ctx.set_gpr(2, error::kCanNotWait);
        return;
    }
    thread->wait = wait;
    if (wait.timeout_address != 0u)
        thread->wait.deadline_us = now_us_ + runtime_->memory().load32(wait.timeout_address);
    AllegrexContext saved = ctx;
    saved.set_gpr(2, result);
    saved.pc = ctx.gpr[31];
    save_current(saved, ThreadStatus::Waiting);
    if (trace_enabled())
        std::cerr << "[kernel] block uid=" << thread->uid << " name=" << thread->name
                  << " wait=" << wait_name(wait.type) << " object=" << wait.object << "\n";
    current_uid_ = 0;
    schedule(ctx);
}

void Kernel::wake(Thread &thread, std::uint32_t result) {
    if (thread.wait.timeout_address != 0u && thread.wait.deadline_us) {
        const std::uint64_t remaining = *thread.wait.deadline_us > now_us_ ? *thread.wait.deadline_us - now_us_ : 0u;
        runtime_->memory().store32(thread.wait.timeout_address, static_cast<std::uint32_t>(remaining));
    }
    thread.context.set_gpr(2, result);
    make_ready(thread);
}

void Kernel::delay_current(AllegrexContext &ctx, std::uint64_t microseconds, std::uint32_t result) {
    WaitState wait{};
    wait.type = WaitType::Delay;
    wait.deadline_us = now_us_ + std::max<std::uint64_t>(microseconds, 1u);
    block(ctx, wait, result);
}

void Kernel::wait_host(AllegrexContext &ctx, std::optional<std::uint64_t> timeout_us, HostWaitPoll poll) {
    // Already satisfied: no thread switch.
    if (const auto result = poll(false)) {
        finish(ctx, *result);
        return;
    }
    if (timeout_us && *timeout_us == 0u) {
        finish(ctx, *poll(true));
        return;
    }
    WaitState wait{};
    wait.type = WaitType::Host;
    if (timeout_us) wait.deadline_us = now_us_ + *timeout_us;
    wait.host_poll = std::move(poll);
    // The poll result replaces v0 when the wait ends.
    block(ctx, wait, 0u);
}

void Kernel::schedule(AllegrexContext &ctx) {
    if (poll_hook_) poll_hook_();
    for (;;) {
        if (runtime_->stopped()) return;
        process_timers();
        if (interrupts_enabled_ && !pending_interrupts_.empty()) {
            interrupted_idle_ = true;
            if (begin_pending_interrupt(ctx)) return;
        }
        if (Thread *best = best_ready_thread()) {
            switch_to(ctx, *best);
            return;
        }

        const auto next = next_event_us();
        if (!next || idle_vblanks_ > kIdleVBlankLimit) {
            std::string report = "PSP scheduler deadlock: no runnable thread";
            for (const auto &[uid, thread] : threads_) {
                report += "\n  uid=" + std::to_string(uid) + " name=" + thread->name +
                    " status=" + status_name(thread->status);
                if (thread->status == ThreadStatus::Waiting)
                    report += std::string(" wait=") + wait_name(thread->wait.type) +
                        " object=" + std::to_string(thread->wait.object) +
                        " resume=" + psprecomp::hex32(thread->context.pc);
            }
            psprecomp::set_runtime_thread_identity(kIdleIdentity, "idle");
            ctx.pc = kIdleStub;
            runtime_->stop(report);
            return;
        }
        advance_clock(*next);
    }
}

void Kernel::advance_clock(std::uint64_t target_us) {
    if (target_us <= now_us_) return;
    const std::uint64_t step_us = target_us - now_us_;
    now_us_ = target_us;
    if (pace_to_real_time()) return;
    // Unpaced, idle time costs nothing, so a thread waiting for the network
    // would see its PSP timeout expire long before a reply could arrive. Let
    // real time pass with the emulated time the wait covers.
    const bool host_wait = std::any_of(threads_.begin(), threads_.end(), [](const auto &entry) {
        return entry.second->status == ThreadStatus::Waiting && entry.second->wait.type == WaitType::Host;
    });
    if (host_wait) std::this_thread::sleep_for(std::chrono::microseconds(std::min(step_us, kHostWaitPollUs)));
}

bool Kernel::pace_to_real_time() {
    // Virtual time jumps to the next event whenever every thread waits, so
    // without this the game runs as fast as frames can be presented: two to
    // three times PSP speed on a 60-90 Hz display. Runs without a window, and
    // the unthrottled setting (GUNGNIR_UNTHROTTLED), keep the unpaced clock.
    static const bool windowed = std::getenv("GUNGNIR_NO_RENDER") == nullptr;
    const bool unthrottled = std::getenv("GUNGNIR_UNTHROTTLED") != nullptr;
    if (!pacing_enabled_ || !windowed || unthrottled) {
        // Turning pacing back on starts from the current moment.
        pacing_started_ = false;
        return false;
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    const double speed = this->speed();
    if (!pacing_started_ || speed != pacing_speed_) {
        // Starting, or the host changed the multiple: hold from this moment
        // rather than charging the new speed for time already spent.
        pacing_started_ = true;
        pacing_speed_ = speed;
        pacing_real_base_ = now;
        pacing_virtual_base_ = now_us_;
        return true;
    }
    const std::int64_t real_us = std::chrono::duration_cast<std::chrono::microseconds>(now - pacing_real_base_).count();
    const std::int64_t virtual_us = static_cast<std::int64_t>(now_us_ - pacing_virtual_base_);
    const PaceStep step = pace_step(virtual_us, real_us, speed);
    // Ahead of real time at this speed: wait. More than a tenth of a second
    // behind (a slow frame, a load): drop the debt instead of racing to make
    // it up.
    if (step.rebase) {
        pacing_real_base_ = now;
        pacing_virtual_base_ = now_us_;
    } else if (step.sleep_us > 0) {
        const Clock::time_point wake = now + std::chrono::microseconds(step.sleep_us);
        Clock::time_point sleep_start = now;
        if (idle_hook_) {
            idle_hook_(wake);
            sleep_start = Clock::now();
        }
        if (sleep_start < wake) std::this_thread::sleep_until(wake);
    }
    return true;
}

double Kernel::speed() const {
    if (!speed_provider_) return 1.0;
    const double value = speed_provider_();
    // A provider that reports a non-positive or non-finite multiple is a
    // programming error; run at PSP speed rather than stopping the clock.
    return std::isfinite(value) && value > 0.0 ? value : 1.0;
}

std::int64_t Kernel::paced_real_us(std::int64_t virtual_us, double speed) noexcept {
    if (!(speed > 0.0)) speed = 1.0;
    return static_cast<std::int64_t>(
        std::llround(static_cast<double>(virtual_us) / speed));
}

Kernel::PaceStep Kernel::pace_step(
    std::int64_t virtual_us, std::int64_t real_us, double speed) noexcept {
    const std::int64_t ahead_us = paced_real_us(virtual_us, speed) - real_us;
    constexpr std::int64_t kMinSleepUs = 1000;
    constexpr std::int64_t kMaxSleepUs = 100000;
    constexpr std::int64_t kMaxLagUs = 100000;
    PaceStep step;
    if (ahead_us >= kMinSleepUs) {
        step.sleep_us = std::min(ahead_us, kMaxSleepUs);
    } else if (ahead_us < -kMaxLagUs) {
        step.rebase = true;
    }
    return step;
}

std::optional<std::uint64_t> Kernel::next_event_us() const {
    std::optional<std::uint64_t> next = next_vblank_us_;
    const auto consider = [&](std::uint64_t value) {
        if (!next || value < *next) next = value;
    };
    for (const auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread->status != ThreadStatus::Waiting) continue;
        if (thread->wait.deadline_us) consider(*thread->wait.deadline_us);
        if (thread->wait.type == WaitType::Host) consider(now_us_ + kHostWaitPollUs);
    }
    for (const auto &[uid, timer] : vtimers) {
        (void)uid;
        if (timer.active && timer.handler != 0u && timer.schedule_us >= vtimer_value(timer))
            consider(now_us_ + (timer.schedule_us - vtimer_value(timer)));
    }
    return next;
}

void Kernel::process_timers() {
    while (now_us_ >= next_vblank_us_) {
        next_vblank_us_ += kVBlankPeriodUs;
        if (current_uid_ == 0) ++idle_vblanks_;
        on_vblank();
    }
    for (auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread->status != ThreadStatus::Waiting) continue;
        if (thread->wait.type == WaitType::Host) {
            const bool timed_out = thread->wait.deadline_us && *thread->wait.deadline_us <= now_us_;
            std::optional<std::uint32_t> result = thread->wait.host_poll(timed_out);
            if (timed_out && !result) result = error::kWaitTimeout;
            if (result) {
                thread->wait.host_poll = nullptr;
                thread->context.set_gpr(2, *result);
                make_ready(*thread);
            }
            continue;
        }
        if (thread->wait.type == WaitType::Audio) {
            // Audio waits have no timeout: the deadline is when the buffer
            // drains, and the poll retries the output at that moment.
            if (thread->wait.deadline_us && *thread->wait.deadline_us <= now_us_) {
                const std::uint32_t result =
                    thread->wait.host_poll ? thread->wait.host_poll(true).value_or(0u) : 0u;
                thread->wait.host_poll = nullptr;
                thread->wait.cancel_hook = nullptr;
                thread->context.set_gpr(2, result);
                make_ready(*thread);
            }
            continue;
        }
        if (thread->wait.deadline_us && *thread->wait.deadline_us <= now_us_) finish_wait_timeout(*thread);
    }
    for (auto &[uid, timer] : vtimers) {
        if (!timer.active || timer.handler == 0u || vtimer_value(timer) < timer.schedule_us) continue;
        const SceUID timer_uid = uid;
        auto &memory = runtime_->memory();
        const std::uint64_t schedule = timer.schedule_us;
        const std::uint64_t current = vtimer_value(timer);
        memory.store32(kVTimerClockScratch, static_cast<std::uint32_t>(schedule));
        memory.store32(kVTimerClockScratch + 4u, static_cast<std::uint32_t>(schedule >> 32u));
        memory.store32(kVTimerClockScratch + 8u, static_cast<std::uint32_t>(current));
        memory.store32(kVTimerClockScratch + 12u, static_cast<std::uint32_t>(current >> 32u));
        InterruptCall call{};
        call.function = timer.handler;
        call.arguments = {
            static_cast<std::uint32_t>(timer_uid), kVTimerClockScratch, kVTimerClockScratch + 8u, timer.common};
        // The handler returns the delay until the next call, measured from the
        // schedule it fired at; 0 stops it.
        call.on_return = [timer_uid, schedule](std::uint32_t next) {
            auto found = kernel().vtimers.find(timer_uid);
            if (found == kernel().vtimers.end()) return;
            if (next == 0u) {
                found->second.handler = 0u;
                return;
            }
            found->second.schedule_us = schedule + next;
        };
        // Disarm until the handler returns, so it cannot fire again meanwhile.
        timer.schedule_us = UINT64_MAX;
        queue_interrupt(std::move(call));
    }
}

void Kernel::finish_wait_timeout(Thread &thread) {
    if (thread.wait.type == WaitType::Delay) {
        make_ready(thread);
        return;
    }
    remove_waiter(thread);
    if (thread.wait.timeout_address != 0u) runtime_->memory().store32(thread.wait.timeout_address, 0u);
    thread.context.set_gpr(2, error::kWaitTimeout);
    make_ready(thread);
}

void Kernel::remove_waiter(Thread &thread) {
    const auto erase = [&](std::deque<SceUID> &waiters) {
        waiters.erase(std::remove(waiters.begin(), waiters.end(), thread.uid), waiters.end());
    };
    switch (thread.wait.type) {
    case WaitType::Semaphore:
        if (auto found = semaphores.find(thread.wait.object); found != semaphores.end()) erase(found->second.waiters);
        break;
    case WaitType::EventFlag:
        if (auto found = event_flags.find(thread.wait.object); found != event_flags.end()) erase(found->second.waiters);
        break;
    case WaitType::Mutex:
        if (auto found = mutexes.find(thread.wait.object); found != mutexes.end()) erase(found->second.waiters);
        break;
    case WaitType::Fpl:
        if (auto found = fpls.find(thread.wait.object); found != fpls.end()) erase(found->second.waiters);
        break;
    case WaitType::MsgPipeSend:
        if (auto found = msgpipes.find(thread.wait.object); found != msgpipes.end())
            erase(found->second.send_waiters);
        break;
    case WaitType::MsgPipeReceive:
        if (auto found = msgpipes.find(thread.wait.object); found != msgpipes.end())
            erase(found->second.receive_waiters);
        break;
    case WaitType::Audio:
        if (thread.wait.cancel_hook) thread.wait.cancel_hook();
        break;
    default:
        break;
    }
}

std::string Kernel::describe_threads() const {
    std::string report = "threads (virtual time " + std::to_string(now_us_ / 1000u) + " ms, vblanks " +
        std::to_string(vblank_count_) + "):";
    for (const auto &[uid, thread] : threads_) {
        report += "\n  uid=" + std::to_string(uid) + " " + thread->name +
            " prio=" + psprecomp::hex32(thread->priority) + " " + status_name(thread->status);
        if (thread->status == ThreadStatus::Waiting) {
            report +=
                std::string(" wait=") + wait_name(thread->wait.type) + " object=" + std::to_string(thread->wait.object);
            if (thread->wait.type == WaitType::EventFlag) {
                report +=
                    " bits=" + psprecomp::hex32(thread->wait.value) + " mode=" + psprecomp::hex32(thread->wait.mode);
                const auto flag = event_flags.find(thread->wait.object);
                if (flag != event_flags.end()) report += " pattern=" + psprecomp::hex32(flag->second.pattern);
            }
            if (thread->wait.type == WaitType::Semaphore) {
                const auto sema = semaphores.find(thread->wait.object);
                report += " want=" + std::to_string(thread->wait.value);
                if (sema != semaphores.end())
                    report += " count=" + std::to_string(sema->second.count) + " name=" + sema->second.name;
            }
            if (thread->wait.deadline_us)
                report += " deadline=" + std::to_string(*thread->wait.deadline_us / 1000u) + "ms";
        }
        if (thread->status != ThreadStatus::Dormant) report += " pc=" + psprecomp::hex32(thread->context.pc);
    }
    return report;
}

void Kernel::on_starvation(AllegrexContext &ctx) {
    if (interrupt_active_ || current_thread() == nullptr) return;
    advance_clock(now_us_ + kStarvationQuantumUs);
    process_timers();
    if (interrupts_enabled_ && !pending_interrupts_.empty()) {
        interrupted_context_ = ctx;
        interrupted_idle_ = false;
        (void)begin_pending_interrupt(ctx);
        return;
    }
    if (!dispatch_enabled_) return;
    Thread *best = best_ready_thread();
    if (best != nullptr && best->priority <= current_thread()->priority) {
        save_current(ctx, ThreadStatus::Ready);
        switch_to(ctx, *best);
    }
}

// ---------------------------------------------------------------------------
// Synchronization helpers

void Kernel::cancel_waiters(std::deque<SceUID> &waiters, std::uint32_t result) {
    for (const SceUID uid : waiters) {
        if (Thread *thread = find_thread(uid); thread != nullptr && thread->status == ThreadStatus::Waiting)
            wake(*thread, result);
    }
    waiters.clear();
}

void Kernel::release_semaphore_waiters(SceUID uid) {
    auto found = semaphores.find(uid);
    if (found == semaphores.end()) return;
    Semaphore &sema = found->second;
    while (!sema.waiters.empty()) {
        Thread *thread = find_thread(sema.waiters.front());
        if (thread == nullptr || thread->status != ThreadStatus::Waiting) {
            sema.waiters.pop_front();
            continue;
        }
        const auto requested = static_cast<std::int32_t>(thread->wait.value);
        if (requested > sema.count) break;
        sema.count -= requested;
        sema.waiters.pop_front();
        wake(*thread, 0u);
    }
}

bool Kernel::event_flag_matches(std::uint32_t pattern, std::uint32_t bits, std::uint32_t mode) noexcept {
    return (mode & 1u) != 0u ? (pattern & bits) != 0u : (pattern & bits) == bits;
}

void Kernel::release_event_flag_waiters(SceUID uid) {
    auto found = event_flags.find(uid);
    if (found == event_flags.end()) return;
    EventFlag &flag = found->second;
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();) {
        Thread *thread = find_thread(*it);
        if (thread == nullptr || thread->status != ThreadStatus::Waiting) {
            it = flag.waiters.erase(it);
            continue;
        }
        if (!event_flag_matches(flag.pattern, thread->wait.value, thread->wait.mode)) {
            ++it;
            continue;
        }
        if (thread->wait.out_address != 0u) runtime_->memory().store32(thread->wait.out_address, flag.pattern);
        if ((thread->wait.mode & 0x10u) != 0u)
            flag.pattern = 0u;
        else if ((thread->wait.mode & 0x20u) != 0u)
            flag.pattern &= ~thread->wait.value;
        it = flag.waiters.erase(it);
        wake(*thread, 0u);
    }
}

void Kernel::release_mutex_waiters(SceUID uid) {
    auto found = mutexes.find(uid);
    if (found == mutexes.end()) return;
    Mutex &mutex = found->second;
    while (mutex.lock_count == 0 && !mutex.waiters.empty()) {
        Thread *thread = find_thread(mutex.waiters.front());
        mutex.waiters.pop_front();
        if (thread == nullptr || thread->status != ThreadStatus::Waiting) continue;
        mutex.owner = thread->uid;
        mutex.lock_count = static_cast<std::int32_t>(thread->wait.value);
        if (mutex.workarea != 0u) {
            runtime_->memory().store32(mutex.workarea, static_cast<std::uint32_t>(mutex.lock_count));
            runtime_->memory().store32(mutex.workarea + 4u, static_cast<std::uint32_t>(mutex.owner));
            runtime_->memory().store32(
                mutex.workarea + 12u, static_cast<std::uint32_t>(mutex.waiters.size()));
        }
        wake(*thread, 0u);
    }
}

void Kernel::wake_thread_end_waiters(SceUID uid, std::int32_t status) {
    for (auto &[other_uid, other] : threads_) {
        (void)other_uid;
        if (other->status == ThreadStatus::Waiting && other->wait.type == WaitType::ThreadEnd &&
            other->wait.object == uid)
            wake(*other, static_cast<std::uint32_t>(status));
    }
}

void Kernel::release_fpl_waiters(SceUID uid) {
    auto found = fpls.find(uid);
    if (found == fpls.end()) return;
    Fpl &fpl = found->second;
    while (!fpl.waiters.empty()) {
        Thread *thread = find_thread(fpl.waiters.front());
        if (thread == nullptr || thread->status != ThreadStatus::Waiting) {
            fpl.waiters.pop_front();
            continue;
        }
        const int index = take_fpl_block(fpl);
        if (index < 0) break;
        const std::uint32_t address = fpl.pool_address + fpl.aligned_size * static_cast<std::uint32_t>(index);
        if (thread->wait.out_address != 0u) runtime_->memory().store32(thread->wait.out_address, address);
        fpl.waiters.pop_front();
        wake(*thread, 0u);
    }
}

void Kernel::pump_msgpipe(SceUID uid) {
    auto found = msgpipes.find(uid);
    if (found == msgpipes.end()) return;
    MsgPipe &pipe = found->second;

    const auto valid_waiter = [this](std::deque<SceUID> &queue) -> Thread * {
        while (!queue.empty()) {
            Thread *thread = find_thread(queue.front());
            if (thread != nullptr && thread->status == ThreadStatus::Waiting) return thread;
            queue.pop_front();
        }
        return nullptr;
    };
    const auto complete = [this, &pipe](std::deque<SceUID> &queue, Thread &thread) {
        const std::uint32_t transferred = thread.wait.total - thread.wait.value;
        if (thread.wait.out_address != 0u) runtime_->memory().store32(thread.wait.out_address, transferred);
        queue.pop_front();
        wake(thread, 0u);
    };

    for (;;) {
        bool moved = false;
        if (pipe.buffer_size == 0u) {
            // Zero-length pipe: senders and receivers meet directly.
            while (!pipe.send_waiters.empty() && !pipe.receive_waiters.empty()) {
                Thread *sender = valid_waiter(pipe.send_waiters);
                Thread *receiver = valid_waiter(pipe.receive_waiters);
                if (sender == nullptr || receiver == nullptr) break;
                const std::uint32_t amount = std::min(sender->wait.value, receiver->wait.value);
                if (amount == 0u) break;
                copy_guest_to_guest(
                    runtime_->memory(), receiver->wait.buffer_address, sender->wait.buffer_address, amount);
                sender->wait.value -= amount;
                sender->wait.buffer_address += amount;
                receiver->wait.value -= amount;
                receiver->wait.buffer_address += amount;
                if (sender->wait.value == 0u || sender->wait.mode == 1u) {
                    complete(pipe.send_waiters, *sender);
                    moved = true;
                }
                if (receiver->wait.value == 0u || receiver->wait.mode == 1u) {
                    complete(pipe.receive_waiters, *receiver);
                    moved = true;
                }
            }
        } else {
            while (!pipe.buffer.empty() && !pipe.receive_waiters.empty()) {
                Thread *receiver = valid_waiter(pipe.receive_waiters);
                if (receiver == nullptr) break;
                const std::uint32_t amount =
                    std::min<std::uint32_t>(receiver->wait.value, static_cast<std::uint32_t>(pipe.buffer.size()));
                if (amount == 0u) break;
                for (std::uint32_t i = 0u; i < amount; ++i)
                    runtime_->memory().store8(receiver->wait.buffer_address + i, pipe.buffer[i]);
                pipe.buffer.erase(pipe.buffer.begin(), pipe.buffer.begin() + amount);
                receiver->wait.value -= amount;
                receiver->wait.buffer_address += amount;
                if (receiver->wait.value == 0u || receiver->wait.mode == 1u) {
                    complete(pipe.receive_waiters, *receiver);
                    moved = true;
                } else {
                    break;
                }
            }
            while (!pipe.send_waiters.empty() && pipe.buffer.size() < pipe.buffer_size) {
                Thread *sender = valid_waiter(pipe.send_waiters);
                if (sender == nullptr) break;
                const std::uint32_t space =
                    pipe.buffer_size - static_cast<std::uint32_t>(pipe.buffer.size());
                const std::uint32_t amount = std::min(sender->wait.value, space);
                if (amount == 0u) break;
                for (std::uint32_t i = 0u; i < amount; ++i)
                    pipe.buffer.push_back(runtime_->memory().load8(sender->wait.buffer_address + i));
                sender->wait.value -= amount;
                sender->wait.buffer_address += amount;
                if (sender->wait.value == 0u || sender->wait.mode == 1u) {
                    complete(pipe.send_waiters, *sender);
                    moved = true;
                } else {
                    break;
                }
            }
        }
        if (!moved) break;
    }
}

std::uint64_t Kernel::vtimer_value(const VTimer &timer) const noexcept {
    return timer.active ? timer.accumulated_us + (now_us_ - timer.base_us) : timer.accumulated_us;
}

// ---------------------------------------------------------------------------
// Memory

std::int32_t Kernel::allocate_block(
    const std::string &name, std::uint32_t type, std::uint32_t size, std::uint32_t address_or_alignment) {
    if (size == 0u) return static_cast<std::int32_t>(error::kIllegalArgument);
    std::uint32_t alignment = kMemoryGranularity;
    if ((type == 3u || type == 4u) && address_or_alignment != 0u) {
        if ((address_or_alignment & (address_or_alignment - 1u)) != 0u)
            return static_cast<std::int32_t>(error::kIllegalArgument);
        alignment = std::max(alignment, address_or_alignment);
    }
    const std::uint32_t aligned_size = align_up(size, kMemoryGranularity);

    std::optional<std::uint32_t> address;
    if (type == 0u || type == 3u) {
        for (const FreeRange &range : free_ranges_) {
            const std::uint32_t start = align_up(range.address, alignment);
            if (start + aligned_size <= range.address + range.size) {
                address = start;
                break;
            }
        }
    } else if (type == 1u || type == 4u) {
        for (auto it = free_ranges_.rbegin(); it != free_ranges_.rend(); ++it) {
            const std::uint64_t end = static_cast<std::uint64_t>(it->address) + it->size;
            if (end < aligned_size) continue;
            const std::uint32_t start = static_cast<std::uint32_t>(end - aligned_size) & ~(alignment - 1u);
            if (start >= it->address) {
                address = start;
                break;
            }
        }
    } else if (type == 2u) {
        const std::uint32_t start = address_or_alignment & ~(kMemoryGranularity - 1u);
        for (const FreeRange &range : free_ranges_) {
            if (start >= range.address && start + aligned_size <= range.address + range.size) {
                address = start;
                break;
            }
        }
    } else {
        return static_cast<std::int32_t>(error::kIllegalMemblockType);
    }
    if (!address) return static_cast<std::int32_t>(error::kNoMemory);

    // Carve [address, address + aligned_size) out of its free range.
    for (auto it = free_ranges_.begin(); it != free_ranges_.end(); ++it) {
        if (*address < it->address || *address + aligned_size > it->address + it->size) continue;
        const FreeRange before{it->address, *address - it->address};
        const FreeRange after{*address + aligned_size, it->address + it->size - (*address + aligned_size)};
        it = free_ranges_.erase(it);
        if (after.size != 0u) it = free_ranges_.insert(it, after);
        if (before.size != 0u) free_ranges_.insert(it, before);
        break;
    }
    const SceUID uid = allocate_uid();
    blocks_.emplace(uid, MemoryBlock{name, *address, aligned_size});
    if (trace_enabled())
        std::cerr << "[kernel] alloc " << name << " size=" << psprecomp::hex32(size) << " -> "
                  << psprecomp::hex32(*address) << " uid=" << uid << "\n";
    return uid;
}

std::int32_t Kernel::free_block(SceUID uid) {
    const auto found = blocks_.find(uid);
    if (found == blocks_.end()) return static_cast<std::int32_t>(error::kIllegalMemblock);
    FreeRange freed{found->second.address, found->second.size};
    blocks_.erase(found);
    auto position = std::lower_bound(free_ranges_.begin(), free_ranges_.end(), freed,
        [](const FreeRange &a, const FreeRange &b) { return a.address < b.address; });
    position = free_ranges_.insert(position, freed);
    // Coalesce with neighbours.
    if (position + 1 != free_ranges_.end() && position->address + position->size == (position + 1)->address) {
        position->size += (position + 1)->size;
        free_ranges_.erase(position + 1);
    }
    if (position != free_ranges_.begin() && (position - 1)->address + (position - 1)->size == position->address) {
        (position - 1)->size += position->size;
        free_ranges_.erase(position);
    }
    return 0;
}

const MemoryBlock *Kernel::find_block(SceUID uid) const {
    const auto found = blocks_.find(uid);
    return found != blocks_.end() ? &found->second : nullptr;
}

std::uint32_t Kernel::free_memory() const noexcept {
    std::uint32_t total = 0u;
    for (const FreeRange &range : free_ranges_) total += range.size;
    return total;
}

// ---------------------------------------------------------------------------
// Interrupts

void Kernel::queue_interrupt(InterruptCall call) {
    pending_interrupts_.push_back(std::move(call));
}

void Kernel::notify_callback(SceUID callback, std::uint32_t argument) {
    const auto found = callbacks.find(callback);
    if (found == callbacks.end()) return;
    ++found->second.notify_count;
    found->second.notify_argument = argument;
    found->second.pending = true;
    if (trace_enabled())
        std::cerr << "[kernel] notify callback " << callback << " " << found->second.name
                  << " arg=" << psprecomp::hex32(argument) << "\n";
}

bool Kernel::deliver_callbacks() {
    Thread *thread = current_thread();
    if (thread == nullptr || interrupt_active_) return false;
    bool queued = false;
    for (auto &[uid, callback] : callbacks) {
        if (!callback.pending || callback.owner != thread->uid || callback.function == 0u) continue;
        callback.pending = false;
        InterruptCall call{};
        call.function = callback.function;
        // PSP callbacks receive (notify count, notify argument, common pointer).
        call.arguments = {callback.notify_count, callback.notify_argument, callback.argument, 0u};
        callback.notify_count = 0u;
        queue_interrupt(std::move(call));
        queued = true;
        if (trace_enabled()) std::cerr << "[kernel] deliver callback " << uid << " " << callback.name << "\n";
    }
    return queued;
}

void Kernel::on_vblank() {
    for (const auto &hook : vblank_hooks_) hook();
    ++vblank_count_;
    for (auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread->status != ThreadStatus::Waiting || thread->wait.type != WaitType::VBlank) continue;
        // Multi-vblank waits park the count in `value`; `wake` restores the
        // result the handler saved in the thread context.
        if (thread->wait.value > 1u) {
            --thread->wait.value;
            continue;
        }
        wake(*thread, thread->context.gpr[2]);
    }
    const auto handlers = sub_interrupts.find(kVBlankInterrupt);
    if (handlers == sub_interrupts.end()) return;
    for (const auto &[sub, handler] : handlers->second) {
        if (!handler.enabled || handler.handler == 0u) continue;
        static bool logged = false;
        if (trace_enabled() && !logged) {
            logged = true;
            std::cerr << "[kernel] dispatch vblank handler sub=" << sub
                      << " handler=" << psprecomp::hex32(handler.handler) << "\n";
        }
        InterruptCall call{};
        call.function = handler.handler;
        call.arguments = {sub, handler.argument, 0u, 0u};
        queue_interrupt(std::move(call));
    }
}

bool Kernel::begin_pending_interrupt(AllegrexContext &ctx) {
    if (pending_interrupts_.empty()) return false;
    InterruptCall call = std::move(pending_interrupts_.front());
    pending_interrupts_.pop_front();
    interrupt_active_ = true;
    interrupt_on_return_ = std::move(call.on_return);
    ctx = pristine_context_;
    for (std::uint32_t i = 0; i < 4u; ++i) ctx.set_gpr(4u + i, call.arguments[i]);
    ctx.set_gpr(29, kInterruptStackTop - kThreadArgumentHome);
    ctx.set_gpr(31, kInterruptReturnStub);
    ctx.pc = call.function;
    psprecomp::set_runtime_thread_identity(kInterruptIdentity, "interrupt");
    return true;
}

void Kernel::interrupt_return_stub(AllegrexContext &ctx) {
    const std::uint32_t result = ctx.gpr[2];
    interrupt_active_ = false;
    if (interrupt_on_return_) {
        auto on_return = std::move(interrupt_on_return_);
        interrupt_on_return_ = nullptr;
        on_return(result);
    }
    if (interrupts_enabled_ && begin_pending_interrupt(ctx)) return;

    if (interrupted_idle_ || current_thread() == nullptr) {
        interrupted_idle_ = false;
        current_uid_ = 0;
        schedule(ctx);
        return;
    }
    ctx = interrupted_context_;
    Thread *current = current_thread();
    psprecomp::set_runtime_thread_identity(current->uid, current->name);
    if (!dispatch_enabled_) return;
    Thread *best = best_ready_thread();
    if (best != nullptr && best->priority < current->priority) {
        save_current(ctx, ThreadStatus::Ready);
        switch_to(ctx, *best);
    }
}

void Kernel::thread_exit_stub(AllegrexContext &ctx) {
    exit_current_thread(ctx, static_cast<std::int32_t>(ctx.gpr[2]),
        current_thread() != nullptr && current_thread()->name == "module_start");
}

void Kernel::call_guest(AllegrexContext &ctx, std::uint32_t function, const std::array<std::uint32_t, 4> &arguments,
    GuestCallReturn on_return) {
    guest_calls_[current_uid_].push_back(GuestCall{ctx.gpr[31], std::move(on_return)});
    for (std::uint32_t i = 0; i < 4u; ++i) ctx.set_gpr(4u + i, arguments[i]);
    ctx.set_gpr(31, kGuestCallReturnStub);
    // A pc other than the import's own tells the import wrapper not to return.
    ctx.pc = function;
}

void Kernel::guest_call_return_stub(AllegrexContext &ctx) {
    auto found = guest_calls_.find(current_uid_);
    if (found == guest_calls_.end() || found->second.empty()) {
        runtime_->stop("guest call returned in a thread that made none");
        return;
    }
    GuestCall call = std::move(found->second.back());
    found->second.pop_back();
    if (found->second.empty()) guest_calls_.erase(found);
    ctx.set_gpr(31, call.return_address);
    ctx.pc = call.return_address;
    call.on_return(ctx, ctx.gpr[2]);
}

void Kernel::idle_stub(AllegrexContext &ctx) {
    current_uid_ = 0;
    schedule(ctx);
}

// ---------------------------------------------------------------------------
// Snapshot export/import (T15b). Deterministic LE encoding; maps iterate in
// UID order (std::map), vectors in index order. Host-only state (poll/cancel
// closures, vblank hooks, pacing base, guest-call continuations, pending
// interrupt payloads) is never serialized: quiesce first, so export fails
// loudly when such state is live.

namespace {

void kput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void kput_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
}

void kput_str(std::vector<std::uint8_t> &out, const std::string &text) {
    kput_u32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

struct KReader {
    const std::uint8_t *p{};
    std::size_t left{};
    bool ok{true};

    [[nodiscard]] std::uint32_t u32() {
        if (left < 4u) {
            ok = false;
            return 0u;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
        p += 4;
        left -= 4;
        return v;
    }

    [[nodiscard]] std::uint64_t u64() {
        if (left < 8u) {
            ok = false;
            return 0u;
        }
        std::uint64_t v = 0u;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        left -= 8;
        return v;
    }

    [[nodiscard]] std::uint8_t u8() {
        if (left < 1u) {
            ok = false;
            return 0u;
        }
        const std::uint8_t v = *p;
        ++p;
        --left;
        return v;
    }

    [[nodiscard]] std::string str() {
        const std::uint32_t len = u32();
        if (!ok || left < len) {
            ok = false;
            return {};
        }
        std::string text(reinterpret_cast<const char *>(p), len);
        p += len;
        left -= len;
        return text;
    }

    [[nodiscard]] bool consumed() const { return ok && left == 0u; }
};

void kput_uids(std::vector<std::uint8_t> &out, const std::deque<SceUID> &waiters) {
    kput_u32(out, static_cast<std::uint32_t>(waiters.size()));
    for (SceUID uid : waiters) kput_u32(out, static_cast<std::uint32_t>(uid));
}

bool kget_uids(KReader &in, std::deque<SceUID> &waiters) {
    const std::uint32_t count = in.u32();
    if (!in.ok || count > 4096u) return false;
    for (std::uint32_t i = 0; i < count; ++i)
        waiters.push_back(static_cast<SceUID>(in.u32()));
    return in.ok;
}

void kput_wait(std::vector<std::uint8_t> &out, const WaitState &wait) {
    kput_u32(out, static_cast<std::uint32_t>(wait.type));
    kput_u32(out, static_cast<std::uint32_t>(wait.object));
    kput_u32(out, wait.value);
    kput_u32(out, wait.mode);
    kput_u32(out, wait.out_address);
    kput_u32(out, wait.timeout_address);
    kput_u32(out, wait.buffer_address);
    kput_u32(out, wait.total);
    kput_u32(out, wait.deadline_us ? 1u : 0u);
    kput_u64(out, wait.deadline_us ? *wait.deadline_us : 0u);
}

bool kget_wait(KReader &in, WaitState &wait) {
    wait.type = static_cast<WaitType>(in.u32());
    wait.object = static_cast<SceUID>(in.u32());
    wait.value = in.u32();
    wait.mode = in.u32();
    wait.out_address = in.u32();
    wait.timeout_address = in.u32();
    wait.buffer_address = in.u32();
    wait.total = in.u32();
    const std::uint32_t has_deadline = in.u32();
    const std::uint64_t deadline = in.u64();
    if (!in.ok) return false;
    if (static_cast<int>(wait.type) < 0 || wait.type > WaitType::Host) return false;
    if (has_deadline > 1u) return false;
    if (has_deadline != 0u) wait.deadline_us = deadline;
    wait.host_poll = nullptr;
    wait.cancel_hook = nullptr;
    return true;
}

} // namespace

void Kernel::restore_clock(
    std::uint64_t now_us, std::uint64_t next_vblank_us, std::uint64_t vblank_count) noexcept {
    now_us_ = now_us;
    next_vblank_us_ = next_vblank_us;
    vblank_count_ = vblank_count;
    resync_real_time();
}

bool Kernel::export_snapshot_kernel(std::vector<std::vector<std::uint8_t>> &sections_out, std::string &error) const {
    if (interrupt_active_) {
        error = "snapshot_busy: interrupt active at save";
        return false;
    }
    if (!pending_interrupts_.empty()) {
        error = "snapshot_busy: pending interrupts at save";
        return false;
    }
    for (const auto &[uid, calls] : guest_calls_) {
        if (!calls.empty()) {
            error = "snapshot_busy: guest call in flight at save";
            return false;
        }
    }
    for (const auto &[uid, thread] : threads_) {
        // Audio waits are rebuildable: hle_audio reattaches the drain poll
        // and the parked-marker cancel hook from the channel index on load
        // (T15c). Host waits have no such owner and are never serializable.
        if (thread->wait.type == WaitType::Host) {
            error = "snapshot_host_wait: thread uid=" + std::to_string(uid) + " has host wait";
            return false;
        }
        if (thread->wait.type != WaitType::Audio &&
            (thread->wait.host_poll != nullptr || thread->wait.cancel_hook != nullptr)) {
            error = "snapshot_host_wait: thread uid=" + std::to_string(uid) + " has unexpected host closure";
            return false;
        }
    }

    std::map<SceUID, int> seen;
    auto note = [&](SceUID uid, const char *what) -> bool {
        if (seen.count(uid) != 0u) {
            error = std::string("snapshot_duplicate_uid: uid=") + std::to_string(uid) + " (" + what + ")";
            return false;
        }
        seen[uid] = 1;
        return true;
    };
    for (const auto &[uid, thread] : threads_)
        if (!note(uid, "thread")) return false;
    for (const auto &[uid, object] : semaphores)
        if (!note(uid, "sema")) return false;
    for (const auto &[uid, object] : event_flags)
        if (!note(uid, "event")) return false;
    for (const auto &[uid, object] : mutexes)
        if (!note(uid, "mutex")) return false;
    for (const auto &[uid, object] : vpls)
        if (!note(uid, "vpl")) return false;
    for (const auto &[uid, object] : fpls)
        if (!note(uid, "fpl")) return false;
    for (const auto &[uid, object] : msgpipes)
        if (!note(uid, "msgpipe")) return false;
    for (const auto &[uid, object] : callbacks)
        if (!note(uid, "callback")) return false;
    for (const auto &[uid, object] : vtimers)
        if (!note(uid, "vtimer")) return false;
    for (const auto &[uid, object] : blocks_)
        if (!note(uid, "block")) return false;
    for (const auto &[uid, max] : seen) {
        (void)max;
        if (next_uid_ <= uid) {
            error = "snapshot_duplicate_uid: next_uid reuses uid=" + std::to_string(uid);
            return false;
        }
    }

    sections_out.clear();
    sections_out.reserve(12);

    {
        std::vector<std::uint8_t> kids;
        kput_u32(kids, static_cast<std::uint32_t>(next_uid_));
        kput_u32(kids, static_cast<std::uint32_t>(current_uid_));
        kput_u64(kids, next_ready_sequence_);
        kput_u64(kids, idle_vblanks_);
        kids.push_back(interrupts_enabled_ ? 1u : 0u);
        kids.push_back(dispatch_enabled_ ? 1u : 0u);
        sections_out.push_back(std::move(kids));
    }

    {
        std::vector<std::uint8_t> threads;
        kput_u32(threads, static_cast<std::uint32_t>(threads_.size()));
        for (const auto &[uid, thread] : threads_) {
            kput_u32(threads, static_cast<std::uint32_t>(uid));
            kput_str(threads, thread->name);
            kput_u32(threads, thread->entry);
            kput_u32(threads, thread->priority);
            kput_u32(threads, thread->initial_priority);
            kput_u32(threads, thread->attributes);
            kput_u32(threads, thread->stack_size);
            kput_u32(threads, thread->stack_bottom);
            kput_u32(threads, thread->control_block);
            kput_u32(threads, thread->gp);
            kput_u32(threads, static_cast<std::uint32_t>(thread->stack_block));
            kput_u32(threads, static_cast<std::uint32_t>(thread->status));
            gungnir::session::encode_context(thread->context, threads);
            kput_wait(threads, thread->wait);
            kput_u32(threads, static_cast<std::uint32_t>(thread->exit_status));
            kput_u32(threads, thread->wakeup_count);
            kput_u64(threads, thread->ready_sequence);
        }
        sections_out.push_back(std::move(threads));
    }

    {
        std::vector<std::uint8_t> sema;
        kput_u32(sema, static_cast<std::uint32_t>(semaphores.size()));
        for (const auto &[uid, object] : semaphores) {
            kput_u32(sema, static_cast<std::uint32_t>(uid));
            kput_str(sema, object.name);
            kput_u32(sema, object.attributes);
            kput_u32(sema, static_cast<std::uint32_t>(object.count));
            kput_u32(sema, static_cast<std::uint32_t>(object.max_count));
            kput_uids(sema, object.waiters);
        }
        sections_out.push_back(std::move(sema));
    }

    {
        std::vector<std::uint8_t> ev;
        kput_u32(ev, static_cast<std::uint32_t>(event_flags.size()));
        for (const auto &[uid, object] : event_flags) {
            kput_u32(ev, static_cast<std::uint32_t>(uid));
            kput_str(ev, object.name);
            kput_u32(ev, object.attributes);
            kput_u32(ev, object.pattern);
            kput_uids(ev, object.waiters);
        }
        sections_out.push_back(std::move(ev));
    }

    {
        std::vector<std::uint8_t> mutex;
        kput_u32(mutex, static_cast<std::uint32_t>(mutexes.size()));
        for (const auto &[uid, object] : mutexes) {
            kput_u32(mutex, static_cast<std::uint32_t>(uid));
            kput_str(mutex, object.name);
            kput_u32(mutex, object.attributes);
            kput_u32(mutex, static_cast<std::uint32_t>(object.owner));
            kput_u32(mutex, static_cast<std::uint32_t>(object.lock_count));
            kput_uids(mutex, object.waiters);
            kput_u32(mutex, object.workarea);
        }
        sections_out.push_back(std::move(mutex));
    }

    {
        std::vector<std::uint8_t> vpl;
        kput_u32(vpl, static_cast<std::uint32_t>(vpls.size()));
        for (const auto &[uid, object] : vpls) {
            kput_u32(vpl, static_cast<std::uint32_t>(uid));
            kput_str(vpl, object.name);
            kput_u32(vpl, object.attributes);
            kput_u32(vpl, object.pool_address);
            kput_u32(vpl, object.pool_size);
            kput_u32(vpl, static_cast<std::uint32_t>(object.pool_block));
            kput_u32(vpl, static_cast<std::uint32_t>(object.allocations.size()));
            for (const auto &[address, size] : object.allocations) {
                kput_u32(vpl, address);
                kput_u32(vpl, size);
            }
        }
        sections_out.push_back(std::move(vpl));
    }

    {
        std::vector<std::uint8_t> fpl;
        kput_u32(fpl, static_cast<std::uint32_t>(fpls.size()));
        for (const auto &[uid, object] : fpls) {
            kput_u32(fpl, static_cast<std::uint32_t>(uid));
            kput_str(fpl, object.name);
            kput_u32(fpl, object.attributes);
            kput_u32(fpl, object.block_size);
            kput_u32(fpl, object.block_count);
            kput_u32(fpl, object.aligned_size);
            kput_u32(fpl, object.pool_address);
            kput_u32(fpl, static_cast<std::uint32_t>(object.pool_block));
            kput_u32(fpl, static_cast<std::uint32_t>(object.used.size()));
            for (bool bit : object.used) fpl.push_back(bit ? 1u : 0u);
            kput_uids(fpl, object.waiters);
        }
        sections_out.push_back(std::move(fpl));
    }

    {
        std::vector<std::uint8_t> pipe;
        kput_u32(pipe, static_cast<std::uint32_t>(msgpipes.size()));
        for (const auto &[uid, object] : msgpipes) {
            kput_u32(pipe, static_cast<std::uint32_t>(uid));
            kput_str(pipe, object.name);
            kput_u32(pipe, object.attributes);
            kput_u32(pipe, object.buffer_size);
            kput_u32(pipe, static_cast<std::uint32_t>(object.buffer.size()));
            pipe.insert(pipe.end(), object.buffer.begin(), object.buffer.end());
            kput_u32(pipe, static_cast<std::uint32_t>(object.buffer_block));
            kput_uids(pipe, object.send_waiters);
            kput_uids(pipe, object.receive_waiters);
        }
        sections_out.push_back(std::move(pipe));
    }

    {
        std::vector<std::uint8_t> cb;
        kput_u32(cb, static_cast<std::uint32_t>(callbacks.size()));
        for (const auto &[uid, object] : callbacks) {
            kput_u32(cb, static_cast<std::uint32_t>(uid));
            kput_str(cb, object.name);
            kput_u32(cb, object.function);
            kput_u32(cb, object.argument);
            kput_u32(cb, static_cast<std::uint32_t>(object.owner));
            kput_u32(cb, object.notify_count);
            kput_u32(cb, object.notify_argument);
            cb.push_back(object.pending ? 1u : 0u);
        }
        sections_out.push_back(std::move(cb));
    }

    {
        std::vector<std::uint8_t> timer;
        kput_u32(timer, static_cast<std::uint32_t>(vtimers.size()));
        for (const auto &[uid, object] : vtimers) {
            kput_u32(timer, static_cast<std::uint32_t>(uid));
            kput_str(timer, object.name);
            timer.push_back(object.active ? 1u : 0u);
            kput_u64(timer, object.base_us);
            kput_u64(timer, object.accumulated_us);
            kput_u64(timer, object.schedule_us);
            kput_u32(timer, object.handler);
            kput_u32(timer, object.common);
        }
        sections_out.push_back(std::move(timer));
    }

    {
        std::vector<std::uint8_t> blocks;
        kput_u32(blocks, static_cast<std::uint32_t>(blocks_.size()));
        for (const auto &[uid, object] : blocks_) {
            kput_u32(blocks, static_cast<std::uint32_t>(uid));
            kput_str(blocks, object.name);
            kput_u32(blocks, object.address);
            kput_u32(blocks, object.size);
        }
        kput_u32(blocks, static_cast<std::uint32_t>(free_ranges_.size()));
        for (const auto &range : free_ranges_) {
            kput_u32(blocks, range.address);
            kput_u32(blocks, range.size);
        }
        sections_out.push_back(std::move(blocks));
    }

    {
        std::vector<std::uint8_t> sub;
        kput_u32(sub, static_cast<std::uint32_t>(sub_interrupts.size()));
        for (const auto &[irq, inner] : sub_interrupts) {
            kput_u32(sub, irq);
            kput_u32(sub, static_cast<std::uint32_t>(inner.size()));
            for (const auto &[subno, handler] : inner) {
                kput_u32(sub, subno);
                kput_u32(sub, handler.handler);
                kput_u32(sub, handler.argument);
                sub.push_back(handler.enabled ? 1u : 0u);
            }
        }
        sections_out.push_back(std::move(sub));
    }

    return true;
}

bool Kernel::import_snapshot_kernel(
    const std::map<std::uint32_t, std::vector<std::uint8_t>> &sections, std::string &error) {
    constexpr std::uint32_t want[12] = {5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u};
    for (std::uint32_t tag : want) {
        if (sections.count(tag) == 0u) {
            error = "snapshot_incomplete: missing kernel section tag=" + std::to_string(tag);
            return false;
        }
    }

    SceUID next_uid = 0;
    SceUID current_uid = 0;
    std::uint64_t next_ready = 0u;
    std::uint64_t idle_vblanks = 0u;
    bool interrupts_enabled = true;
    bool dispatch_enabled = true;
    {
        const auto &bytes = sections.at(5u);
        KReader in{bytes.data(), bytes.size()};
        next_uid = static_cast<SceUID>(in.u32());
        current_uid = static_cast<SceUID>(in.u32());
        next_ready = in.u64();
        idle_vblanks = in.u64();
        const std::uint8_t irq = in.u8();
        const std::uint8_t disp = in.u8();
        if (!in.consumed() || irq > 1u || disp > 1u) {
            error = "snapshot_bad_length: kids section malformed";
            return false;
        }
        interrupts_enabled = irq != 0u;
        dispatch_enabled = disp != 0u;
    }

    std::map<SceUID, std::unique_ptr<Thread>> threads;
    {
        const auto &bytes = sections.at(6u);
        KReader in{bytes.data(), bytes.size()};
        const std::uint32_t count = in.u32();
        if (!in.ok || count > 256u) {
            error = "snapshot_truncated: threads count malformed";
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            auto thread = std::make_unique<Thread>();
            const SceUID uid = static_cast<SceUID>(in.u32());
            thread->uid = uid;
            thread->name = in.str();
            thread->entry = in.u32();
            thread->priority = in.u32();
            thread->initial_priority = in.u32();
            thread->attributes = in.u32();
            thread->stack_size = in.u32();
            thread->stack_bottom = in.u32();
            thread->control_block = in.u32();
            thread->gp = in.u32();
            thread->stack_block = static_cast<SceUID>(in.u32());
            thread->status = static_cast<ThreadStatus>(in.u32());
            if (!in.ok || static_cast<int>(thread->status) < 0 || thread->status > ThreadStatus::Dead) {
                error = "snapshot_bad_length: thread status malformed";
                return false;
            }
            if (thread->name.size() > 256u) {
                error = "snapshot_bad_length: thread name too long";
                return false;
            }
            std::array<std::uint8_t, 848> ctx_bytes{};
            if (in.left < ctx_bytes.size()) {
                error = "snapshot_truncated: thread context truncated";
                return false;
            }
            std::span<const std::uint8_t> ctx_span(in.p, ctx_bytes.size());
            std::string ctx_error;
            gungnir::session::SnapshotError cerr{};
            if (!gungnir::session::decode_context(ctx_span, thread->context, cerr)) {
                error = "snapshot_bad_length: thread context malformed";
                return false;
            }
            in.p += ctx_bytes.size();
            in.left -= ctx_bytes.size();
            if (!kget_wait(in, thread->wait)) {
                error = "snapshot_bad_length: thread wait malformed";
                return false;
            }
            thread->exit_status = static_cast<std::int32_t>(in.u32());
            thread->wakeup_count = in.u32();
            thread->ready_sequence = in.u64();
            if (!in.ok) {
                error = "snapshot_truncated: thread entry truncated";
                return false;
            }
            if (threads.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate thread uid=" + std::to_string(uid);
                return false;
            }
            threads[uid] = std::move(thread);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in threads section";
            return false;
        }
    }

    auto decode_map_header = [&](std::uint32_t tag, KReader &in, std::uint32_t &count, std::string &why) -> bool {
        const auto found = sections.find(tag);
        if (found == sections.end()) {
            why = "snapshot_incomplete: missing kernel section tag=" + std::to_string(tag);
            return false;
        }
        in = KReader{found->second.data(), found->second.size()};
        count = in.u32();
        if (!in.ok || count > 4096u) {
            why = "snapshot_truncated: section tag=" + std::to_string(tag) + " count malformed";
            return false;
        }
        return true;
    };

    std::map<SceUID, Semaphore> semaphores;
    std::map<SceUID, EventFlag> event_flags;
    std::map<SceUID, Mutex> mutexes;
    std::map<SceUID, Vpl> vpls;
    std::map<SceUID, Fpl> fpls;
    std::map<SceUID, MsgPipe> msgpipes;
    std::map<SceUID, Callback> callbacks;
    std::map<SceUID, VTimer> vtimers;
    std::map<SceUID, MemoryBlock> blocks;
    std::vector<FreeRange> free_ranges;
    std::map<std::uint32_t, std::map<std::uint32_t, SubInterruptHandler>> sub_interrupts;

    std::string why;
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(7u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            Semaphore object;
            object.name = in.str();
            object.attributes = in.u32();
            object.count = static_cast<std::int32_t>(in.u32());
            object.max_count = static_cast<std::int32_t>(in.u32());
            if (!kget_uids(in, object.waiters) || !in.ok) {
                error = "snapshot_truncated: sema entry truncated";
                return false;
            }
            if (semaphores.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate sema uid=" + std::to_string(uid);
                return false;
            }
            semaphores[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in sema section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(8u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            EventFlag object;
            object.name = in.str();
            object.attributes = in.u32();
            object.pattern = in.u32();
            if (!kget_uids(in, object.waiters) || !in.ok) {
                error = "snapshot_truncated: event entry truncated";
                return false;
            }
            if (event_flags.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate event uid=" + std::to_string(uid);
                return false;
            }
            event_flags[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in event section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(9u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            Mutex object;
            object.name = in.str();
            object.attributes = in.u32();
            object.owner = static_cast<SceUID>(in.u32());
            object.lock_count = static_cast<std::int32_t>(in.u32());
            if (!kget_uids(in, object.waiters) || !in.ok) {
                error = "snapshot_truncated: mutex entry truncated";
                return false;
            }
            object.workarea = in.u32();
            if (!in.ok) {
                error = "snapshot_truncated: mutex entry truncated";
                return false;
            }
            if (mutexes.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate mutex uid=" + std::to_string(uid);
                return false;
            }
            mutexes[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in mutex section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(10u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            Vpl object;
            object.name = in.str();
            object.attributes = in.u32();
            object.pool_address = in.u32();
            object.pool_size = in.u32();
            object.pool_block = static_cast<SceUID>(in.u32());
            const std::uint32_t allocs = in.u32();
            if (!in.ok || allocs > 65536u) {
                error = "snapshot_truncated: vpl entry truncated";
                return false;
            }
            for (std::uint32_t a = 0; a < allocs; ++a) {
                const std::uint32_t address = in.u32();
                const std::uint32_t size = in.u32();
                if (!in.ok) {
                    error = "snapshot_truncated: vpl entry truncated";
                    return false;
                }
                object.allocations[address] = size;
            }
            if (vpls.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate vpl uid=" + std::to_string(uid);
                return false;
            }
            vpls[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in vpl section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(11u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            Fpl object;
            object.name = in.str();
            object.attributes = in.u32();
            object.block_size = in.u32();
            object.block_count = in.u32();
            object.aligned_size = in.u32();
            object.pool_address = in.u32();
            object.pool_block = static_cast<SceUID>(in.u32());
            const std::uint32_t used = in.u32();
            if (!in.ok || used > 65536u || in.left < used) {
                error = "snapshot_truncated: fpl entry truncated";
                return false;
            }
            object.used.reserve(used);
            for (std::uint32_t b = 0; b < used; ++b) {
                const std::uint8_t bit = in.u8();
                if (!in.ok || bit > 1u) {
                    error = "snapshot_bad_length: fpl used bit malformed";
                    return false;
                }
                object.used.push_back(bit != 0u);
            }
            if (!kget_uids(in, object.waiters) || !in.ok) {
                error = "snapshot_truncated: fpl entry truncated";
                return false;
            }
            if (fpls.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate fpl uid=" + std::to_string(uid);
                return false;
            }
            fpls[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in fpl section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(12u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            MsgPipe object;
            object.name = in.str();
            object.attributes = in.u32();
            object.buffer_size = in.u32();
            const std::uint32_t buffered = in.u32();
            if (!in.ok || buffered > 1048576u || in.left < buffered) {
                error = "snapshot_truncated: msgpipe entry truncated";
                return false;
            }
            object.buffer.assign(in.p, in.p + buffered);
            in.p += buffered;
            in.left -= buffered;
            object.buffer_block = static_cast<SceUID>(in.u32());
            if (!kget_uids(in, object.send_waiters) || !kget_uids(in, object.receive_waiters) || !in.ok) {
                error = "snapshot_truncated: msgpipe entry truncated";
                return false;
            }
            if (msgpipes.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate msgpipe uid=" + std::to_string(uid);
                return false;
            }
            msgpipes[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in msgpipe section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(13u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            Callback object;
            object.name = in.str();
            object.function = in.u32();
            object.argument = in.u32();
            object.owner = static_cast<SceUID>(in.u32());
            object.notify_count = in.u32();
            object.notify_argument = in.u32();
            const std::uint8_t pending = in.u8();
            if (!in.ok || pending > 1u) {
                error = "snapshot_bad_length: callback entry malformed";
                return false;
            }
            object.pending = pending != 0u;
            if (callbacks.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate callback uid=" + std::to_string(uid);
                return false;
            }
            callbacks[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in callback section";
            return false;
        }
    }
    {
        KReader in{nullptr, 0};
        std::uint32_t count = 0u;
        if (!decode_map_header(14u, in, count, why)) {
            error = why;
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            VTimer object;
            object.name = in.str();
            const std::uint8_t active = in.u8();
            if (!in.ok || active > 1u) {
                error = "snapshot_bad_length: vtimer entry malformed";
                return false;
            }
            object.active = active != 0u;
            object.base_us = in.u64();
            object.accumulated_us = in.u64();
            object.schedule_us = in.u64();
            object.handler = in.u32();
            object.common = in.u32();
            if (!in.ok) {
                error = "snapshot_truncated: vtimer entry truncated";
                return false;
            }
            if (vtimers.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate vtimer uid=" + std::to_string(uid);
                return false;
            }
            vtimers[uid] = std::move(object);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in vtimer section";
            return false;
        }
    }
    {
        const auto &bytes = sections.at(15u);
        KReader in{bytes.data(), bytes.size()};
        const std::uint32_t count = in.u32();
        if (!in.ok || count > 4096u) {
            error = "snapshot_truncated: memblocks count malformed";
            return false;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const SceUID uid = static_cast<SceUID>(in.u32());
            MemoryBlock object;
            object.name = in.str();
            object.address = in.u32();
            object.size = in.u32();
            if (!in.ok) {
                error = "snapshot_truncated: memblock entry truncated";
                return false;
            }
            if (blocks.count(uid) != 0u) {
                error = "snapshot_duplicate_uid: duplicate block uid=" + std::to_string(uid);
                return false;
            }
            blocks[uid] = std::move(object);
        }
        const std::uint32_t ranges = in.u32();
        if (!in.ok || ranges > 4096u) {
            error = "snapshot_truncated: free ranges malformed";
            return false;
        }
        for (std::uint32_t i = 0; i < ranges; ++i) {
            FreeRange range;
            range.address = in.u32();
            range.size = in.u32();
            if (!in.ok) {
                error = "snapshot_truncated: free range truncated";
                return false;
            }
            free_ranges.push_back(range);
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in memblocks section";
            return false;
        }
    }
    {
        const auto &bytes = sections.at(16u);
        KReader in{bytes.data(), bytes.size()};
        const std::uint32_t outer = in.u32();
        if (!in.ok || outer > 32u) {
            error = "snapshot_truncated: subint count malformed";
            return false;
        }
        for (std::uint32_t o = 0; o < outer; ++o) {
            const std::uint32_t irq = in.u32();
            const std::uint32_t inner = in.u32();
            if (!in.ok || inner > 32u) {
                error = "snapshot_truncated: subint inner malformed";
                return false;
            }
            for (std::uint32_t s = 0; s < inner; ++s) {
                const std::uint32_t subno = in.u32();
                SubInterruptHandler handler;
                handler.handler = in.u32();
                handler.argument = in.u32();
                const std::uint8_t enabled = in.u8();
                if (!in.ok || enabled > 1u) {
                    error = "snapshot_bad_length: subint entry malformed";
                    return false;
                }
                handler.enabled = enabled != 0u;
                sub_interrupts[irq][subno] = handler;
            }
        }
        if (!in.consumed()) {
            error = "snapshot_truncated: trailing bytes in subint section";
            return false;
        }
    }

    std::map<SceUID, int> seen;
    auto note = [&](SceUID uid) -> bool {
        if (seen.count(uid) != 0u) {
            error = "snapshot_duplicate_uid: cross-map duplicate uid=" + std::to_string(uid);
            return false;
        }
        seen[uid] = 1;
        return true;
    };
    for (const auto &[uid, thread] : threads)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : semaphores)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : event_flags)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : mutexes)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : vpls)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : fpls)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : msgpipes)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : callbacks)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : vtimers)
        if (!note(uid)) return false;
    for (const auto &[uid, object] : blocks)
        if (!note(uid)) return false;
    for (const auto &[uid, max] : seen) {
        (void)max;
        if (next_uid <= uid) {
            error = "snapshot_duplicate_uid: next_uid reuses uid=" + std::to_string(uid);
            return false;
        }
    }
    if (current_uid != 0 && threads.count(current_uid) == 0u) {
        error = "snapshot_incomplete: current thread missing uid=" + std::to_string(current_uid);
        return false;
    }

    threads_.clear();
    for (auto &[uid, thread] : threads) threads_[uid] = std::move(thread);
    this->semaphores = std::move(semaphores);
    this->event_flags = std::move(event_flags);
    this->mutexes = std::move(mutexes);
    this->vpls = std::move(vpls);
    this->fpls = std::move(fpls);
    this->msgpipes = std::move(msgpipes);
    this->callbacks = std::move(callbacks);
    this->vtimers = std::move(vtimers);
    blocks_ = std::move(blocks);
    free_ranges_ = std::move(free_ranges);
    this->sub_interrupts = std::move(sub_interrupts);
    next_uid_ = next_uid;
    current_uid_ = current_uid;
    next_ready_sequence_ = next_ready;
    idle_vblanks_ = idle_vblanks;
    interrupts_enabled_ = interrupts_enabled;
    dispatch_enabled_ = dispatch_enabled;
    pending_interrupts_.clear();
    guest_calls_.clear();
    interrupt_active_ = false;
    interrupted_idle_ = false;
    interrupt_on_return_ = nullptr;
    resync_real_time();
    return true;
}

} // namespace gungnir::hle

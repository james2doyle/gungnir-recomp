// T6b fixtures: kernel memory, threads, synchronization and IO paths.
//
// Usage: hle_kernel_tests --nids CSV [--iso FILE] [--ms DIR]
// Exit 0 on success, 1 on a failed contract, 2 on usage.

#include "hle/hle_common.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>

namespace {

using gungnir::hle::AllegrexContext;
using gungnir::hle::HleRegistrar;
using gungnir::hle::Kernel;
using gungnir::hle::Runtime;
using gungnir::hle::kernel;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::filesystem::path test_nids_path() {
    const char *path = std::getenv("GUNGNIR_TEST_NIDS_CSV");
    return path != nullptr ? std::filesystem::path(path) : std::filesystem::path();
}

class Fixture {
public:
    static constexpr std::uint32_t kText = 0x08810000u;
    static constexpr std::uint32_t kOutput = 0x08811000u;

    Fixture(const std::filesystem::path &nids, const std::filesystem::path &iso,
        const std::filesystem::path &memory_stick)
        : runtime(32u * 1024u * 1024u) {
        kernel() = Kernel{};
        kernel().install(runtime, 0x08801000u, 0x08820000u);
        kernel().set_pacing_enabled(false);
        runtime.nids().load_csv(nids);
        for (const auto &symbol : runtime.nids().all())
            nids_by_name[{symbol.library, symbol.name}] = symbol.nid;
        HleRegistrar hle(runtime);
        gungnir::hle::register_sysmem(hle);
        gungnir::hle::register_threadman(hle);
        gungnir::hle::register_system(hle);
        if (!iso.empty()) gungnir::hle::register_io(hle, iso, memory_stick);
        kernel().start_loader_thread(ctx, 0x08820000u, 0u);
        write_string(kText, "contract");
    }

    ~Fixture() {
        kernel() = Kernel{};
        psprecomp::set_runtime_starvation_hook(nullptr, 0);
    }

    void write_string(std::uint32_t address, std::string_view text) {
        gungnir::hle::write_cstring(runtime.memory(), address, text, 512u);
    }

    std::uint32_t call(const std::string &library, const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        for (unsigned i = 0; i < 8u; ++i) ctx.set_gpr(i + 4u, 0u);
        unsigned index = 0;
        for (const std::uint32_t value : args) ctx.set_gpr(4u + index++, value);
        ctx.set_gpr(31, kReturnAddress);
        const auto found = nids_by_name.find({library, name});
        if (found == nids_by_name.end())
            throw std::runtime_error("no NID for " + library + "::" + name);
        runtime.invoke_import(library, found->second, ctx);
        return ctx.gpr[2];
    }

    std::uint32_t thread(const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        return call("ThreadManForUser", name, args);
    }

    Runtime runtime;
    Kernel &k = kernel();
    AllegrexContext &ctx = runtime.cpu();
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name;
    static constexpr std::uint32_t kReturnAddress = 0x08822000u;
};

void memory_contracts() {
    // No loader thread here: its module_start stack would occupy the top of
    // the user region and move the high-allocation expectation.
    Runtime runtime(32u * 1024u * 1024u);
    kernel() = Kernel{};
    kernel().install(runtime, 0x08801000u, 0x08820000u);
    auto &k = kernel();
    const auto total = k.free_memory();
    check(total > 0u, "free memory reported");

    const auto low = k.allocate_block("low", 0u, 1u, 0u);
    const auto high = k.allocate_block("high", 1u, 257u, 0u);
    check(low > 0 && high > 0, "low/high allocations succeed");
    check(k.find_block(low)->size == 256u, "low allocation rounds to 256 bytes");
    check(k.find_block(high)->address == gungnir::hle::kUserMemoryEnd - 512u,
          "high allocation sits at the top");
    check(static_cast<std::uint32_t>(k.allocate_block("bad", 9u, 16u, 0u)) ==
              gungnir::hle::error::kIllegalMemblockType,
          "unknown partition type rejected");
    check(k.free_block(low) == 0 && k.free_block(high) == 0, "blocks free");
    check(static_cast<std::uint32_t>(k.free_block(low)) ==
              gungnir::hle::error::kIllegalMemblock,
          "double free rejected");
    check(k.free_memory() == total, "freed fragments coalesce");
}

void thread_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    auto &ctx = f.ctx;
    const auto loader = k.current_uid();
    check(ctx.gpr[28] == 0x08801000u, "loader starts with GP");
    check(ctx.gpr[31] == gungnir::hle::kThreadExitStub, "loader starts with exit stub");

    check(static_cast<std::uint32_t>(k.create_thread("bad", 0x08824001u, 0x30u, 0x1000u, 0u, 0u)) ==
              gungnir::hle::error::kIllegalEntry,
          "unaligned entry rejected");
    check(static_cast<std::uint32_t>(k.create_thread("bad", 0x08824000u, 0u, 0x1000u, 0u, 0u)) ==
              gungnir::hle::error::kIllegalPriority,
          "zero priority rejected");

    const auto high = k.create_thread("high", 0x08824000u, 0x10u, 0x1000u, 0u, 0x08801000u);
    check(high > 0, "thread created");
    check(k.start_thread(ctx, high, 0u, 0u) == 0, "thread started");
    const auto *thread = k.find_thread(high);
    check(thread->context.gpr[28] == 0x08801000u, "thread inherits GP");

    ctx.set_gpr(31, Fixture::kReturnAddress);
    k.finish(ctx, 42u);
    check(k.current_uid() == high, "higher priority thread preempts");
    check(ctx.pc == 0x08824000u, "preempted thread resumes at its entry");
    check(k.find_thread(loader)->context.pc == Fixture::kReturnAddress,
          "loader return address saved");
    check(k.find_thread(loader)->context.gpr[2] == 42u, "loader result saved");

    check(f.thread("sceKernelGetThreadId") == static_cast<std::uint32_t>(high),
          "ThreadMan reports the live thread");
    (void)f.thread("sceKernelDelayThread", {2500u});
    check(k.current_uid() == loader, "delay yields to the loader");
    check(k.find_thread(high)->status == gungnir::hle::ThreadStatus::Waiting,
          "delayed thread is waiting");
    check(k.find_thread(high)->context.gpr[2] == 0u, "delayed thread result is zero");

    k.delay_current(ctx, 3000u, 77u);
    check(k.now_us() == 2500u, "clock stops at the earliest deadline");
    check(k.current_uid() == high, "deadline wakes the delayed thread");
    check(ctx.gpr[2] == 0u, "delay resumes with its saved result");

    k.exit_current_thread(ctx, 7, false);
    check(k.find_thread(high)->exit_status == 7, "exit status retained");
    check(k.current_uid() == loader, "loader resumes after thread exit");
    check(k.delete_thread(high) == 0, "dormant thread deleted");
}

void sync_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &mem = f.runtime.memory();
    const auto sema = f.thread("sceKernelCreateSema", {0u, 0u, 1u, 1u});
    check(static_cast<std::int32_t>(sema) > 0, "semaphore created");
    check(f.thread("sceKernelWaitSema", {sema, 1u}) == 0u, "available semaphore acquired");
    const auto cb_sema = f.thread("sceKernelCreateSema", {0u, 0u, 1u, 1u});
    check(f.thread("sceKernelWaitSemaCB", {cb_sema, 1u}) == 0u, "CB semaphore wait acquires");
    check(f.thread("sceKernelDeleteSema", {cb_sema}) == 0u, "CB semaphore deleted");
    check(static_cast<std::uint32_t>(f.thread("sceKernelPollSema", {sema, 1u})) ==
              gungnir::hle::error::kSemaZero,
          "empty semaphore poll reports zero");
    check(f.thread("sceKernelSignalSema", {sema, 1u}) == 0u, "semaphore signalled");
    check(f.thread("sceKernelPollSema", {sema, 1u}) == 0u, "signalled semaphore acquired");
    check(f.thread("sceKernelSignalSema", {sema, 1u}) == 0u, "semaphore refilled");
    check(static_cast<std::uint32_t>(f.thread("sceKernelSignalSema", {sema, 1u})) ==
              gungnir::hle::error::kSemaOverflow,
          "overflow signalled");
    check(f.thread("sceKernelDeleteSema", {sema}) == 0u, "semaphore deleted");

    const auto flag = f.thread("sceKernelCreateEventFlag", {0u, 0u, 0u});
    check(static_cast<std::int32_t>(flag) > 0, "event flag created");
    check(f.thread("sceKernelSetEventFlag", {flag, 0x5u}) == 0u, "event bits set");
    check(f.thread("sceKernelPollEventFlag", {flag, 0x5u, 0x20u, Fixture::kOutput}) == 0u,
          "OR wait matches");
    check(mem.load32(Fixture::kOutput) == 0x5u, "matched pattern written");
    check(f.thread("sceKernelClearEventFlag", {flag, 0x1u}) == 0u, "event bits cleared");
    check(static_cast<std::uint32_t>(f.thread("sceKernelPollEventFlag",
              {flag, 0x1u, 0x20u, Fixture::kOutput})) == gungnir::hle::error::kEvfCond,
          "missing bit reports condition error");
    check(f.thread("sceKernelDeleteEventFlag", {flag}) == 0u, "event flag deleted");
}

void vpl_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &mem = f.runtime.memory();
    f.write_string(Fixture::kText, "vpl");
    const auto vpl = f.thread("sceKernelCreateVpl", {Fixture::kText, 1u, 0u, 0x1000u, 0u});
    check(static_cast<std::int32_t>(vpl) > 0, "variable pool created");
    check(f.thread("sceKernelTryAllocateVpl", {vpl, 0x100u, Fixture::kOutput}) == 0u,
          "variable allocation succeeds");
    const std::uint32_t first = mem.load32(Fixture::kOutput);
    check(f.thread("sceKernelTryAllocateVpl", {vpl, 0x100u, Fixture::kOutput + 4u}) == 0u,
          "second variable allocation succeeds");
    const std::uint32_t second = mem.load32(Fixture::kOutput + 4u);
    check(first != second, "allocations do not overlap");
    check((first & 0x3Fu) == 0u && (second & 0x3Fu) == 0u, "allocations are 64-byte aligned");

    check(f.thread("sceKernelReferVplStatus", {vpl, Fixture::kOutput + 0x40u}) == 0u,
          "variable pool status reads");
    check(mem.load32(Fixture::kOutput + 0x40u) == 52u, "status struct size");
    check(mem.load32(Fixture::kOutput + 0x40u + 40u) >= 0x1000u, "pool size reported");
    check(mem.load32(Fixture::kOutput + 0x40u + 44u) <= 0x1000u - 0x200u, "free size reported");

    check(f.thread("sceKernelFreeVpl", {vpl, first}) == 0u, "variable allocation freed");
    check(f.thread("sceKernelTryAllocateVpl", {vpl, 0x100u, Fixture::kOutput + 8u}) == 0u,
          "freed space is reusable");
    check(mem.load32(Fixture::kOutput + 8u) == first, "first-fit reuses the freed gap");
    check(static_cast<std::uint32_t>(f.thread("sceKernelTryAllocateVpl",
              {vpl, 0x2000u, Fixture::kOutput + 12u})) == gungnir::hle::error::kIllegalMemsize,
          "oversized allocation rejects");
    check(f.thread("sceKernelDeleteVpl", {vpl}) == 0u, "variable pool deleted");
}

void lwmutex_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &mem = f.runtime.memory();
    const std::uint32_t work = Fixture::kOutput;
    f.write_string(Fixture::kText, "lwmutex");
    check(f.thread("sceKernelCreateLwMutex", {work, Fixture::kText, 0u, 0u, 0u}) == 0u,
          "lightweight mutex creates through its work area");
    const std::uint32_t uid = mem.load32(work + 16u);
    check(static_cast<std::int32_t>(uid) > 0, "work area records the object id");
    check(mem.load32(work) == 0u, "work area starts unlocked");

    check(f.call("Kernel_Library", "sceKernelLockLwMutex", {work, 1u, 0u}) == 0u,
          "lock through the kernel alias library");
    check(mem.load32(work) == 1u, "work area lock level");
    check(mem.load32(work + 4u) == static_cast<std::uint32_t>(f.k.current_uid()),
          "work area owner is the caller");
    check(f.call("Kernel_Library", "sceKernelUnlockLwMutex", {work, 1u}) == 0u, "unlock succeeds");
    check(mem.load32(work) == 0u && mem.load32(work + 4u) == 0u, "work area is unlocked");

    check(f.thread("sceKernelCreateLwMutex", {work, Fixture::kText, 0x200u, 1u, 0u}) == 0u,
          "recursive lightweight mutex creates locked");
    check(mem.load32(work) == 1u && mem.load32(work + 4u) != 0u, "initial lock recorded");
    check(f.call("Kernel_Library", "sceKernelLockLwMutex", {work, 2u, 0u}) == 0u,
          "recursive lock succeeds");
    check(mem.load32(work) == 3u, "recursive lock level accumulates");
    check(f.call("Kernel_Library", "sceKernelUnlockLwMutex", {work, 3u}) == 0u,
          "recursive unlock succeeds");
    check(mem.load32(work) == 0u, "recursive mutex fully unlocked");
    check(f.thread("sceKernelDeleteLwMutex", {work}) == 0u, "lightweight mutex deleted");
    check(mem.load32(work + 16u) == 0xFFFFFFFFu, "deleted work area is cleared");
}

int g_timer_fired = 0;

void timer_handler(Runtime &, AllegrexContext &ctx) {
    ++g_timer_fired;
    kernel().interrupt_return_stub(ctx);
}

void alarm_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    auto &ctx = f.ctx;
    g_timer_fired = 0;
    constexpr std::uint32_t kHandler = 0x08825000u;
    f.runtime.register_function(kHandler, &timer_handler, "timer-handler");
    const auto uid = k.allocate_uid();
    gungnir::hle::VTimer timer{};
    timer.name = "alarm";
    timer.active = true;
    timer.base_us = k.now_us();
    timer.schedule_us = 1000u;
    timer.handler = kHandler;
    k.vtimers[uid] = timer;

    k.delay_current(ctx, 2000u, 0u);
    check(ctx.pc == kHandler, "timer interrupt becomes the next dispatch");
    f.runtime.run(kHandler, 1u);
    check(g_timer_fired == 1, "VTimer handler fired once");
    check(k.now_us() == 2000u, "blocked thread resumes at its own deadline");
    check(k.current_uid() != 0, "loader resumed after the timer");
}

void msgpipe_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    auto &mem = f.runtime.memory();
    const auto loader = k.current_uid();
    f.write_string(Fixture::kText, "pipe");
    f.write_string(Fixture::kOutput, "hello");

    check(static_cast<std::uint32_t>(f.thread("sceKernelCreateMsgPipe",
              {Fixture::kText, 0u, 0u, 16u, 0u})) == gungnir::hle::error::kIllegalArgument,
          "pipe rejects partition 0");
    const auto pipe = f.thread("sceKernelCreateMsgPipe", {Fixture::kText, 1u, 0u, 16u, 0u});
    check(static_cast<std::int32_t>(pipe) > 0, "message pipe created");

    const std::uint32_t recv = Fixture::kOutput + 0x100u;
    const std::uint32_t result = Fixture::kOutput + 0x200u;
    const std::uint32_t status = Fixture::kOutput + 0x300u;

    check(static_cast<std::uint32_t>(f.thread("sceKernelTryReceiveMsgPipe",
              {pipe, recv, 8u, 0u, result})) == gungnir::hle::error::kMppEmpty,
          "empty pipe try receive reports MPP_EMPTY");
    check(f.thread("sceKernelTrySendMsgPipe", {pipe, Fixture::kOutput, 5u, 0u, result}) == 0u,
          "try send succeeds");
    check(mem.load32(result) == 5u, "try send reports five bytes");
    mem.store32(status, 56u);
    check(f.thread("sceKernelReferMsgPipeStatus", {pipe, status}) == 0u, "pipe status readable");
    check(mem.load32(status) == 56u, "pipe status size");
    check(mem.load32(status + 40u) == 16u && mem.load32(status + 44u) == 11u,
          "pipe reports buffer and free size");
    check(static_cast<std::uint32_t>(f.thread("sceKernelTrySendMsgPipe",
              {pipe, Fixture::kOutput, 17u, 0u, result})) == gungnir::hle::error::kIllegalSize,
          "send larger than the pipe is rejected");
    check(f.thread("sceKernelReceiveMsgPipe", {pipe, recv, 5u, 0u, result}) == 0u,
          "blocking receive gets buffered data");
    check(mem.load32(result) == 5u && mem.load8(recv) == 'h' && mem.load8(recv + 4u) == 'o',
          "received bytes match");

    // Fill the pipe, then a lower-priority sender blocks until receives make room.
    f.write_string(Fixture::kOutput, "0123456789abcdef");
    check(f.thread("sceKernelSendMsgPipe", {pipe, Fixture::kOutput, 16u, 0u, result, 0u}) == 0u,
          "pipe fills");
    const auto sender = k.create_thread("sender", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(sender > 0 && k.start_thread(f.ctx, sender, 0u, 0u) == 0, "sender started");
    k.delay_current(f.ctx, 10u, 0u);
    check(k.current_uid() == sender, "sender gets the CPU");
    const std::uint32_t pending = Fixture::kOutput + 0x400u;
    const std::uint32_t pending_result = Fixture::kOutput + 0x500u;
    f.write_string(pending, "ABCDEFGH");
    check(f.thread("sceKernelSendMsgPipe", {pipe, pending, 8u, 0u, pending_result, 0u}) == 0u,
          "blocked send accepted");
    check(k.current_uid() == loader, "blocked send returns to the loader");
    check(k.find_thread(sender)->status == gungnir::hle::ThreadStatus::Waiting,
          "sender waits for space");
    check(f.thread("sceKernelReceiveMsgPipe", {pipe, recv, 4u, 0u, result}) == 0u &&
              mem.load32(result) == 4u,
          "partial receive succeeds");
    check(k.find_thread(sender)->status == gungnir::hle::ThreadStatus::Waiting,
          "sender still waits without full room");
    check(f.thread("sceKernelReceiveMsgPipe", {pipe, recv, 4u, 0u, result}) == 0u,
          "second partial receive");
    check(k.find_thread(sender)->status == gungnir::hle::ThreadStatus::Ready ||
              k.find_thread(sender)->status == gungnir::hle::ThreadStatus::Running,
          "freeing eight bytes wakes the sender");
    check(mem.load32(pending_result) == 8u, "woken sender reports its byte count");
    check(f.thread("sceKernelReceiveMsgPipe", {pipe, recv, 8u, 0u, result}) == 0u &&
              mem.load32(result) == 8u && mem.load8(recv) == '8' && mem.load8(recv + 7u) == 'f',
          "buffered head received in order");
    check(f.thread("sceKernelReceiveMsgPipe", {pipe, recv, 8u, 0u, result}) == 0u &&
              mem.load32(result) == 8u && mem.load8(recv) == 'A' && mem.load8(recv + 7u) == 'H',
          "blocked sender payload received in order");
    check(f.thread("sceKernelDeleteMsgPipe", {pipe}) == 0u, "pipe deleted");
    check(k.terminate_thread(f.ctx, sender, false) == 0, "blocked sender terminated");

    // A timed send returns WAIT_TIMEOUT and writes zero remaining time.
    const auto timed_pipe = f.thread("sceKernelCreateMsgPipe", {Fixture::kText, 1u, 0u, 16u, 0u});
    check(static_cast<std::int32_t>(timed_pipe) > 0, "timed pipe created");
    check(f.thread("sceKernelSendMsgPipe", {timed_pipe, Fixture::kOutput, 16u, 0u, result, 0u}) == 0u,
          "timed pipe fills");
    const auto timed_sender = k.create_thread("timed", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(timed_sender > 0 && k.start_thread(f.ctx, timed_sender, 0u, 0u) == 0, "timed sender started");
    k.delay_current(f.ctx, 10u, 0u);
    const std::uint32_t timeout_address = Fixture::kOutput + 0x600u;
    mem.store32(timeout_address, 1000u);
    check(f.thread("sceKernelSendMsgPipe",
              {timed_pipe, pending, 8u, 0u, result, timeout_address}) == 0u,
          "timed send blocks");
    check(k.current_uid() == loader, "timed send returns to the loader");
    k.delay_current(f.ctx, 5000u, 0u);
    check(k.current_uid() == timed_sender, "timed sender wakes at its deadline");
    check(f.ctx.gpr[2] == gungnir::hle::error::kWaitTimeout, "timed send reports timeout");
    check(mem.load32(timeout_address) == 0u, "timed send clears the remaining time");
    check(f.thread("sceKernelDeleteMsgPipe", {timed_pipe}) == 0u, "timed pipe deleted");
    k.exit_current_thread(f.ctx, 0, false);
    check(k.current_uid() == loader, "loader resumes after the timeout");
}

void fpl_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    auto &mem = f.runtime.memory();
    const auto loader = k.current_uid();
    f.write_string(Fixture::kText, "pool");

    check(static_cast<std::uint32_t>(f.thread("sceKernelCreateFpl",
              {Fixture::kText, 0u, 0u, 32u, 4u, 0u})) == gungnir::hle::error::kIllegalArgument,
          "pool rejects partition 0");
    check(static_cast<std::uint32_t>(f.thread("sceKernelCreateFpl",
              {Fixture::kText, 1u, 0u, 0u, 4u, 0u})) == gungnir::hle::error::kIllegalMemsize,
          "zero block size rejected");
    const auto fpl = f.thread("sceKernelCreateFpl", {Fixture::kText, 1u, 0u, 32u, 4u, 0u});
    check(static_cast<std::int32_t>(fpl) > 0, "fixed pool created");

    const std::uint32_t block_out = Fixture::kOutput;
    const std::uint32_t status = Fixture::kOutput + 0x100u;
    check(f.thread("sceKernelTryAllocateFpl", {fpl, block_out}) == 0u, "first block allocated");
    const std::uint32_t first = mem.load32(block_out);
    check(f.thread("sceKernelTryAllocateFpl", {fpl, block_out}) == 0u, "second block allocated");
    const std::uint32_t second = mem.load32(block_out);
    check(second == first + 32u, "blocks are evenly spaced");
    check(f.thread("sceKernelTryAllocateFpl", {fpl, block_out}) == 0u &&
              f.thread("sceKernelTryAllocateFpl", {fpl, block_out}) == 0u,
          "remaining blocks allocated");
    check(static_cast<std::uint32_t>(f.thread("sceKernelTryAllocateFpl", {fpl, block_out})) ==
              gungnir::hle::error::kNoMemory,
          "empty pool reports no memory");
    mem.store32(status, 56u);
    check(f.thread("sceKernelReferFplStatus", {fpl, status}) == 0u, "pool status readable");
    check(mem.load32(status) == 56u && mem.load32(status + 40u) == 32u &&
              mem.load32(status + 44u) == 4u && mem.load32(status + 48u) == 0u &&
              mem.load32(status + 52u) == 0u,
          "pool status reports size, blocks and waiters");
    check(f.thread("sceKernelFreeFpl", {fpl, first}) == 0u, "block freed");
    check(static_cast<std::uint32_t>(f.thread("sceKernelFreeFpl", {fpl, first})) ==
              gungnir::hle::error::kIllegalMemblock,
          "double free rejected");
    check(f.thread("sceKernelReferFplStatus", {fpl, status}) == 0u &&
              mem.load32(status + 48u) == 1u,
          "free block counted");

    // Refill the pool so the next allocation must wait.
    check(f.thread("sceKernelTryAllocateFpl", {fpl, block_out}) == 0u, "free block reallocated");

    // Blocked allocation wakes when a block is freed.
    const auto waiter = k.create_thread("pool-waiter", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(waiter > 0 && k.start_thread(f.ctx, waiter, 0u, 0u) == 0, "pool waiter started");
    k.delay_current(f.ctx, 10u, 0u);
    check(k.current_uid() == waiter, "pool waiter gets the CPU");
    const std::uint32_t waiter_out = Fixture::kOutput + 0x200u;
    check(f.thread("sceKernelAllocateFpl", {fpl, waiter_out, 0u}) == 0u, "blocking allocate waits");
    check(k.current_uid() == loader, "blocking allocate returns to the loader");
    check(k.find_thread(waiter)->status == gungnir::hle::ThreadStatus::Waiting,
          "pool waiter blocks");
    check(f.thread("sceKernelFreeFpl", {fpl, second}) == 0u, "second block freed");
    check(k.find_thread(waiter)->status == gungnir::hle::ThreadStatus::Ready ||
              k.find_thread(waiter)->status == gungnir::hle::ThreadStatus::Running,
          "freeing a block wakes the waiter");
    check(mem.load32(waiter_out) != 0u, "woken waiter receives a block pointer");
    check(k.terminate_thread(f.ctx, waiter, false) == 0, "woken pool waiter terminated");

    // Cancel wakes remaining waiters with WAIT_CANCEL and reports the count.
    const auto cancel_waiter = k.create_thread("pool-cancel", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(cancel_waiter > 0 && k.start_thread(f.ctx, cancel_waiter, 0u, 0u) == 0, "cancel waiter started");
    k.delay_current(f.ctx, 10u, 0u);
    check(k.current_uid() == cancel_waiter, "cancel waiter gets the CPU");
    const std::uint32_t cancel_out = Fixture::kOutput + 0x300u;
    check(f.thread("sceKernelAllocateFpl", {fpl, cancel_out, 0u}) == 0u, "second waiter blocks");
    check(k.current_uid() == loader, "second block returns to the loader");
    const std::uint32_t count = Fixture::kOutput + 0x400u;
    check(f.thread("sceKernelCancelFpl", {fpl, count}) == 0u, "pool cancel succeeds");
    check(mem.load32(count) == 1u, "cancel reports one waiter");
    check(k.find_thread(cancel_waiter)->context.gpr[2] == gungnir::hle::error::kWaitCancel,
          "cancelled waiter reports WAIT_CANCEL");
    check(f.thread("sceKernelDeleteFpl", {fpl}) == 0u, "pool deleted");
}

void thread_end_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    const auto loader = k.current_uid();
    const std::int32_t worker_uid = k.create_thread("worker", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(worker_uid > 0 && k.start_thread(f.ctx, worker_uid, 0u, 0u) == 0, "worker started");
    const std::uint32_t worker = static_cast<std::uint32_t>(worker_uid);
    check(k.current_uid() == loader, "lower-priority worker does not preempt");
    check(f.thread("sceKernelWaitThreadEnd", {worker, 0u}) == 0u, "thread-end wait blocks");
    check(k.current_uid() == worker, "worker runs while the loader waits");
    (void)f.thread("sceKernelExitThread", {7u});
    check(k.current_uid() == loader, "worker exit returns to the loader");
    check(f.ctx.gpr[2] == 7u, "thread-end wait returns the exit status");
    check(f.thread("sceKernelWaitThreadEnd", {worker, 0u}) == 7u,
          "already-dormant thread returns its status immediately");
    check(static_cast<std::uint32_t>(f.thread("sceKernelWaitThreadEnd", {0u, 0u})) ==
              gungnir::hle::error::kIllegalThid,
          "self/zero thread id rejected");
    check(static_cast<std::uint32_t>(f.thread("sceKernelWaitThreadEnd", {0x777u, 0u})) ==
              gungnir::hle::error::kUnknownThid,
          "unknown thread id rejected");
}

void blocking_sync_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &k = f.k;
    const auto loader = k.current_uid();
    const auto sema = f.thread("sceKernelCreateSema", {0u, 0u, 0u, 1u});
    check(static_cast<std::int32_t>(sema) > 0, "blocking semaphore created");
    const auto waiter = k.create_thread("waiter", 0x08824000u, 0x10u, 0x1000u, 0u, 0x08801000u);
    check(waiter > 0, "waiter thread created");
    check(k.start_thread(f.ctx, waiter, 0u, 0u) == 0, "waiter started");
    f.ctx.set_gpr(31, Fixture::kReturnAddress);
    k.finish(f.ctx, 0u);
    check(k.current_uid() == waiter, "waiter preempts loader");
    (void)f.thread("sceKernelWaitSema", {sema, 1u});
    check(k.current_uid() == loader,
          "blocking wait returns control to the loader");
    check(k.find_thread(waiter)->status == gungnir::hle::ThreadStatus::Waiting,
          "waiter is waiting");
    (void)f.thread("sceKernelSignalSema", {sema, 1u});
    const auto waiter_status = k.find_thread(waiter)->status;
    check(waiter_status == gungnir::hle::ThreadStatus::Ready ||
              waiter_status == gungnir::hle::ThreadStatus::Running,
          "signal makes waiter runnable");
    check(f.thread("sceKernelDeleteSema", {sema}) == 0u, "sema deleted");
}

void io_contracts(const std::filesystem::path &iso, const std::filesystem::path &memory_stick) {
    Fixture f(test_nids_path(), iso, memory_stick);
    auto &mem = f.runtime.memory();
    f.write_string(Fixture::kText, "disc0:/PSP_GAME/PARAM.SFO");
    const auto opened = static_cast<std::int32_t>(f.call("IoFileMgrForUser", "sceIoOpen",
        {Fixture::kText, 1u}));
    check(opened >= 0, "disc PARAM.SFO opens");
    check(f.call("IoFileMgrForUser", "sceIoRead", {static_cast<std::uint32_t>(opened),
              Fixture::kOutput, 4u}) == 4u,
          "disc read returns four bytes");
    const std::uint32_t magic = mem.load32(Fixture::kOutput);
    check(magic == 0x46535000u, "PARAM.SFO magic read correctly");
    check(f.call("IoFileMgrForUser", "sceIoClose",
              {static_cast<std::uint32_t>(opened)}) == 0u,
          "disc file closes");
    f.write_string(Fixture::kText, "ms0:/t6b-test.bin");
    const auto host = static_cast<std::int32_t>(f.call("IoFileMgrForUser", "sceIoOpen",
        {Fixture::kText, 0x0602u}));
    check(host >= 0, "memory-stick file creates");
    f.write_string(Fixture::kOutput, "abc");
    check(f.call("IoFileMgrForUser", "sceIoWrite",
              {static_cast<std::uint32_t>(host), Fixture::kOutput, 3u}) == 3u,
          "memory-stick write succeeds");
    check(f.call("IoFileMgrForUser", "sceIoClose",
              {static_cast<std::uint32_t>(host)}) == 0u,
          "memory-stick file closes");
    check(f.call("IoFileMgrForUser", "sceIoOpen",
              {Fixture::kText, 1u}) >= 0,
          "memory-stick file reopens");
}

} // namespace

int main(int argc, char **argv) {
    std::string nids;
    std::filesystem::path iso;
    std::filesystem::path memory_stick;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--nids" && i + 1 < argc) nids = argv[++i];
        else if (arg == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (arg == "--ms" && i + 1 < argc) memory_stick = argv[++i];
        else {
            std::cerr << "usage: hle_kernel_tests --nids CSV [--iso FILE] [--ms DIR]\n";
            return 2;
        }
    }
    if (nids.empty()) return 2;
    setenv("GUNGNIR_TEST_NIDS_CSV", nids.c_str(), 1);
    try {
        memory_contracts();
        thread_contracts();
        sync_contracts();
        blocking_sync_contracts();
        alarm_contracts();
        vpl_contracts();
        msgpipe_contracts();
        fpl_contracts();
        thread_end_contracts();
        lwmutex_contracts();
        if (!iso.empty()) {
            std::filesystem::create_directories(memory_stick);
            io_contracts(iso, memory_stick);
        }
        std::cout << "hle_kernel_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

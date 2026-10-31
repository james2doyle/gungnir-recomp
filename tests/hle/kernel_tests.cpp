// T6b fixtures: kernel memory, threads, synchronization and IO paths.
//
// Usage: hle_kernel_tests --nids CSV [--iso FILE] [--ms DIR]
// Exit 0 on success, 1 on a failed contract, 2 on usage.

#include "audio/audio_sink.hpp"
#include "hle/hle_atrac.hpp"
#include "hle/hle_audio.hpp"
#include "hle/hle_common.hpp"
#include "audio/atrac_file.hpp"
#include "hle/hle_utility.hpp"
#include "hle/kernel.hpp"
#include "savedata/save_store.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <vector>

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
        gungnir::hle::reset_audio_state();
        gungnir::hle::reset_atrac_state();
        gungnir::hle::reset_utility_state();
        gungnir::hle::register_sysmem(hle);
        gungnir::hle::register_threadman(hle);
        gungnir::hle::register_audio(hle);
        gungnir::hle::register_atrac(hle);
        gungnir::hle::register_system(hle);
        if (!iso.empty() || !memory_stick.empty()) gungnir::hle::register_io(hle, iso, memory_stick);
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
    check(f.thread("sceKernelCheckCallback") == 0u, "no pending callbacks reported");
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

class RecordingSink final : public gungnir::audio::AudioSink {
public:
    struct Recorded {
        std::vector<std::int16_t> samples;
        std::uint32_t frames{};
        std::uint32_t channels{};
        std::uint32_t left_volume{};
        std::uint32_t right_volume{};
        std::uint32_t channel{};
        std::uint64_t start_us{};
    };

    bool submit(const gungnir::audio::AudioBuffer &buffer) override {
        Recorded recorded{};
        recorded.samples.assign(buffer.samples, buffer.samples + buffer.frames * buffer.channels);
        recorded.frames = buffer.frames;
        recorded.channels = buffer.channels;
        recorded.left_volume = buffer.left_volume;
        recorded.right_volume = buffer.right_volume;
        recorded.channel = buffer.channel;
        recorded.start_us = buffer.start_us;
        buffers.push_back(std::move(recorded));
        return true;
    }

    std::vector<Recorded> buffers;
};

void audio_contracts() {
    Fixture f(test_nids_path(), {}, {});
    RecordingSink sink;
    gungnir::audio::set_audio_sink(&sink);
    struct SinkGuard {
        ~SinkGuard() { gungnir::audio::set_audio_sink(nullptr); }
    } guard;
    auto &k = f.k;
    auto &mem = f.runtime.memory();
    const auto loader = k.current_uid();
    const auto audio = [&f](const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return f.call("sceAudio", name, args);
    };
    const std::uint32_t buffer = Fixture::kOutput;
    const std::uint32_t buffer2 = Fixture::kOutput + 0x400u;
    for (std::uint32_t i = 0u; i < 128u; ++i)
        mem.store16(buffer + i * 2u, static_cast<std::uint16_t>(i * 100u));
    for (std::uint32_t i = 0u; i < 128u; ++i)
        mem.store16(buffer2 + i * 2u, static_cast<std::uint16_t>(0x1000u + i));

    check(audio("sceAudioChReserve", {2u, 64u, 0u}) == 2u, "channel reserved");
    check(audio("sceAudioChReserve", {2u, 64u, 0u}) == 0x80260003u, "reserved channel refused");
    check(audio("sceAudioChReserve", {8u, 64u, 0u}) == 0x80260003u, "bad channel refused");
    check(audio("sceAudioChReserve", {3u, 63u, 0u}) == 0x80260006u, "unaligned sample count refused");
    check(audio("sceAudioChReserve", {3u, 64u, 1u}) == 0x80260007u, "bad format refused");
    check(audio("sceAudioOutput", {3u, 0x8000u, buffer}) == 0x80260001u, "unreserved output refused");
    check(audio("sceAudioOutput", {2u, 0x10000u, buffer}) == 0x8026000Bu, "invalid volume refused");
    check(audio("sceAudioOutputPannedBlocking", {2u, 0xFFFFFFFFu, 0u, buffer}) == 0x8026000Bu,
        "negative panned blocking volume refused");
    check(audio("sceAudioOutput", {2u, 0x8000u, buffer}) == 64u, "output starts a buffer");
    check(sink.buffers.size() == 1u && sink.buffers[0].frames == 64u && sink.buffers[0].channels == 2u &&
              sink.buffers[0].left_volume == 0x8000u && sink.buffers[0].channel == 2u &&
              sink.buffers[0].samples[0] == 0 && sink.buffers[0].samples[1] == 100,
        "stereo buffer reached the sink");
    check(audio("sceAudioGetChannelRestLen", {2u}) == 64u, "rest length after output");
    check(audio("sceAudioGetChannelRestLength", {2u}) == 64u, "rest length agrees while a buffer plays");
    check(audio("sceAudioOutput", {2u, 0x8000u, buffer2}) == 0x80260002u, "second output is busy");

    const std::int32_t waiter_uid = k.create_thread("audio", 0x08824000u, 0x30u, 0x1000u, 0u, 0x08801000u);
    check(waiter_uid > 0 && k.start_thread(f.ctx, waiter_uid, 0u, 0u) == 0, "audio waiter started");
    const std::uint32_t waiter = static_cast<std::uint32_t>(waiter_uid);
    k.delay_current(f.ctx, 10u, 0u);
    check(k.current_uid() == waiter, "audio waiter gets the CPU");
    check(audio("sceAudioOutputPannedBlocking", {2u, 0x8000u, 0x8000u, buffer2}) == 0u,
        "blocking output parks the caller");
    check(k.current_uid() == loader, "blocking output returns to the loader");
    check(audio("sceAudioChRelease", {2u}) == 0x80260002u, "release refused while a thread is parked");
    check(audio("sceAudioGetChannelRestLen", {2u}) == 128u, "parked buffer counted once");
    check(audio("sceAudioGetChannelRestLength", {2u}) == 128u, "parked buffer counted by both rest calls");
    k.delay_current(f.ctx, 2000u, 0u);
    check(k.current_uid() == waiter, "blocked output wakes when the buffer drains");
    check(f.ctx.gpr[2] == 64u, "blocked output returns its sample count");
    check(sink.buffers.size() == 2u && sink.buffers[1].samples[0] == 0x1000, "blocked buffer reached the sink");
    k.exit_current_thread(f.ctx, 0, false);
    check(k.current_uid() == loader, "loader resumes after the audio wait");
    check(audio("sceAudioGetChannelRestLen", {2u}) == 64u, "new buffer rest length");
    k.delay_current(f.ctx, 2000u, 0u);
    check(audio("sceAudioGetChannelRestLen", {2u}) == 0u, "buffer drained");

    check(audio("sceAudioOutput", {2u, 0u, 0u}) == 64u, "null output accepted");
    check(audio("sceAudioGetChannelRestLen", {2u}) == 64u, "null drain counted by rest len");
    check(audio("sceAudioGetChannelRestLength", {2u}) == 0u, "null drain excluded by rest length");
    check(audio("sceAudioOutput", {2u, 0u, buffer}) == 64u, "null drain can be replaced");
    check(audio("sceAudioSetChannelDataLen", {2u, 63u}) == 0x80260006u, "unaligned length refused");
    check(audio("sceAudioSetChannelDataLen", {3u, 63u}) == 0x80260006u, "length checked before reservation");
    check(audio("sceAudioChangeChannelConfig", {2u, 1u}) == 0x80260002u, "config refused while playing");
    check(audio("sceAudioChangeChannelVolume", {3u, 0x10000u, 0u}) == 0x8026000Bu, "volume checked before reservation");
    check(audio("sceAudioChangeChannelVolume", {3u, 0x100u, 0x100u}) == 0u, "volume accepted without reservation");
    k.delay_current(f.ctx, 3000u, 0u);
    check(audio("sceAudioChangeChannelConfig", {2u, 0x10u}) == 0u, "mono config accepted when idle");
    check(audio("sceAudioOutput", {2u, 0x8000u, buffer}) == 64u, "mono output starts");
    check(sink.buffers.back().channels == 1u && sink.buffers.back().frames == 64u, "mono buffer staged");
    check(audio("sceAudioChRelease", {2u}) == 0u, "channel released while playing");
    check(audio("sceAudioChRelease", {2u}) == 0x80260008u, "second release refused");
}

void sas_contracts() {
    Fixture f(test_nids_path(), {}, {});
    auto &mem = f.runtime.memory();
    const auto sas = [&f](const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return f.call("sceSasCore", name, args);
    };
    const std::uint32_t core = Fixture::kOutput + 0x1000u; // 64-byte aligned
    const std::uint32_t vag = Fixture::kOutput + 0x800u;
    const std::uint32_t out = Fixture::kOutput;
    const std::uint32_t heights = Fixture::kOutput + 0x2000u;

    // Two 16-byte VAG blocks: one with constant nibble 1 at shift 2
    // (1024 per sample), then a terminator. Predictor 0 keeps the math exact.
    mem.store8(vag, 0x02u);
    mem.store8(vag + 1u, 0x00u);
    for (std::uint32_t i = 2u; i < 16u; ++i) mem.store8(vag + i, 0x11u);
    mem.store8(vag + 16u, 0x00u);
    mem.store8(vag + 17u, 0x07u);
    for (std::uint32_t i = 18u; i < 32u; ++i) mem.store8(vag + i, 0x00u);

    check(sas("__sceSasInit", {0u, 256u, 32u, 0u, 44100u}) == 0x80420005u, "bad core address rejected");
    check(sas("__sceSasInit", {core | 1u, 256u, 32u, 0u, 44100u}) == 0x80420005u, "misaligned core rejected");
    check(sas("__sceSasInit", {core, 60u, 32u, 0u, 44100u}) == 0x80420001u, "bad grain rejected");
    check(sas("__sceSasInit", {core, 256u, 33u, 0u, 44100u}) == 0x80420002u, "bad voice count rejected");
    check(sas("__sceSasInit", {core, 256u, 32u, 2u, 44100u}) == 0x80420003u, "bad output mode rejected");
    check(sas("__sceSasInit", {core, 256u, 32u, 0u, 48000u}) == 0x80420004u, "bad sample rate rejected");
    check(sas("__sceSasInit", {core, 256u, 32u, 0u, 44100u}) == 0u, "SAS core initialised");

    check(sas("__sceSasSetVoice", {core, 32u, vag, 32u, 0u}) == 0x80420010u, "bad voice index rejected");
    check(sas("__sceSasSetVoice", {core, 0u, vag, 0u, 0u}) == 0x80420014u, "zero voice size rejected");
    check(sas("__sceSasSetVoice", {core, 0u, vag, 17u, 0u}) == 0x80420014u, "unaligned voice size rejected");
    check(sas("__sceSasSetVoice", {core, 0u, vag, 32u, 2u}) == 0x80420015u, "bad loop mode rejected");
    check(sas("__sceSasSetPitch", {core, 0u, 0u}) == 0x80420012u, "zero pitch rejected");
    check(sas("__sceSasSetPitch", {core, 0u, 0x4001u}) == 0x80420012u, "pitch above maximum rejected");
    check(sas("__sceSasSetVolume", {core, 0u, 0x1001u, 0u, 0u, 0u}) == 0x80420018u, "volume above maximum rejected");
    check(sas("__sceSasSetVoice", {core, 0u, vag, 32u, 0u}) == 0u, "VAG voice set");
    check(sas("__sceSasSetPitch", {core, 0u, 0x1000u}) == 0u, "unity pitch set");
    check(sas("__sceSasSetVolume", {core, 0u, 0x1000u, 0x1000u, 0u, 0u}) == 0u, "unity volume set");
    check(sas("__sceSasSetKeyOn", {core, 0u}) == 0u, "voice keyed on");
    check(sas("__sceSasSetKeyOn", {core, 0u}) == 0x80420016u, "second key-on rejected");
    check(sas("__sceSasGetEnvelopeHeight", {core, 0u}) == 0x3FFF8000u, "envelope starts full without ADSR");
    check(sas("__sceSasGetAllEnvelopeHeights", {core, heights}) == 0u, "all envelope heights readable");
    check(mem.load32(heights) == 0x3FFF8000u, "first envelope height written");
    check(sas("__sceSasSetPause", {core, 1u, 1u}) == 0u, "voice paused");
    check(sas("__sceSasGetPauseFlag", {core}) == 1u, "pause flag set");
    check(sas("__sceSasSetPause", {core, 1u, 0u}) == 0u, "voice resumed");
    check(sas("__sceSasGetPauseFlag", {core}) == 0u, "pause flag cleared");

    check(sas("__sceSasCore", {core, out}) == 0u, "SAS core rendered");
    check(mem.load16(out) == 1023u && mem.load16(out + 2u) == 1023u, "first frame matches hand-computed VAG");
    check(mem.load16(out + 27u * 4u) == 1023u, "last decoded frame matches");
    check(mem.load16(out + 28u * 4u) == 0u, "frames past the terminator are silent");
    check(sas("__sceSasGetEndFlag", {core}) == 0xFFFFFFFFu, "all voices report ended");
    check(sas("__sceSasGetEnvelopeHeight", {core, 0u}) == 0u, "envelope cleared after the voice ends");

    check(sas("__sceSasGetGrain", {core}) == 256u, "grain size readable");
    check(sas("__sceSasSetGrain", {core, 64u}) == 0u, "grain size updated");
    check(sas("__sceSasGetGrain", {core}) == 64u, "updated grain size readable");
    check(sas("__sceSasSetGrain", {core, 63u}) == 0x80420001u, "bad grain update rejected");
    check(sas("__sceSasSetGrain", {core, 256u}) == 0u, "grain restored");
    check(sas("__sceSasGetOutputmode", {core}) == 0u, "output mode readable");
    check(sas("__sceSasSetOutputmode", {core, 1u}) == 0u, "output mode updated");
    check(sas("__sceSasGetOutputmode", {core}) == 1u, "updated output mode readable");
    check(sas("__sceSasSetOutputmode", {core, 2u}) == 0x80420003u, "bad output mode update rejected");
    check(sas("__sceSasSetOutputmode", {core, 0u}) == 0u, "output mode restored");

    // Mix a fresh voice into an existing stereo buffer with unity gain.
    check(sas("__sceSasSetKeyOn", {core, 0u}) == 0u, "voice keyed on again");
    for (std::uint32_t i = 0u; i < 64u; ++i) mem.store16(out + i * 2u, 100u);
    check(sas("__sceSasCoreWithMix", {core, out, 0x1000u, 0x1000u}) == 0u, "mix render succeeded");
    check(mem.load16(out) == 1123u && mem.load16(out + 2u) == 1123u, "mix adds the hand-computed voice");
}

std::vector<std::uint8_t> atrac_riff(std::uint32_t frames = 1u, std::uint32_t total_samples = 2048u) {
    constexpr std::uint16_t kBlockAlign = 1536u;
    constexpr std::uint16_t kCodecParams = 0x00BFu;
    std::vector<std::uint8_t> data(static_cast<std::size_t>(frames) * kBlockAlign, 0u);
    for (std::uint32_t frame = 0; frame < frames; ++frame) data[frame * kBlockAlign] = 0x60u;
    std::vector<std::uint8_t> out;
    const auto tag = [&out](const char *value) { out.insert(out.end(), value, value + 4); };
    const auto u16 = [&out](std::uint16_t value) {
        out.push_back(static_cast<std::uint8_t>(value));
        out.push_back(static_cast<std::uint8_t>(value >> 8u));
    };
    const auto u32 = [&out](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    tag("RIFF");
    u32(0u);
    tag("WAVE");
    tag("fmt ");
    u32(52u);
    u16(0xFFFEu);
    u16(2u);
    u32(44'100u);
    u32(static_cast<std::uint32_t>(kBlockAlign) * 44'100u / 2048u);
    u16(kBlockAlign);
    u16(0u);
    u16(34u);
    u16(2048u);
    u32(3u);
    static constexpr std::uint8_t kGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
        0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
    out.insert(out.end(), kGuid, kGuid + 16);
    const std::uint8_t extra[12] = {1u, 0u, static_cast<std::uint8_t>(kCodecParams >> 8u),
        static_cast<std::uint8_t>(kCodecParams), 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    out.insert(out.end(), extra, extra + 12);
    tag("fact");
    u32(8u);
    u32(total_samples);
    u32(0u);
    tag("data");
    u32(static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());
    const std::uint32_t size = static_cast<std::uint32_t>(out.size() - 8u);
    for (int i = 0; i < 4; ++i) out[4 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    return out;
}

void atrac_contracts() {
    Fixture f(test_nids_path(), {}, {});
    const auto atrac = [&f](const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return f.call("sceAtrac3plus", name, args);
    };
    check(atrac("sceAtracGetAtracID", {0u}) == 0x80630004u, "bad codec type rejected");
    const std::uint32_t at3 = atrac("sceAtracGetAtracID", {0x1001u});
    const std::uint32_t at3plus = atrac("sceAtracGetAtracID", {0x1000u});
    check(at3 > 0u && at3plus > 0u && at3 != at3plus, "AT3 and AT3+ IDs allocated");
    check(atrac("sceAtracReleaseAtracID", {at3}) == 0u, "AT3 ID released");
    check(atrac("sceAtracReleaseAtracID", {at3}) == 0x80630005u, "double release rejected");
    check(atrac("sceAtracReleaseAtracID", {at3plus}) == 0u, "AT3+ ID released");

    auto &mem = f.runtime.memory();
    const auto riff = atrac_riff();
    constexpr std::uint32_t kRiff = 0x08832000u;
    for (std::size_t i = 0; i < riff.size(); ++i)
        mem.store8(kRiff + static_cast<std::uint32_t>(i), riff[i]);
    const std::uint32_t id = atrac("sceAtracSetDataAndGetID",
        {kRiff, static_cast<std::uint32_t>(riff.size())});
    check(id > 0u && id != 0x80630006u, "ATRAC3+ RIFF opens");
    constexpr std::uint32_t kValue = Fixture::kOutput + 0x20u;
    check(atrac("sceAtracGetChannel", {id, kValue}) == 0u && mem.load32(kValue) == 2u,
        "ATRAC channel count");
    check(atrac("sceAtracGetMaxSample", {id, kValue}) == 0u && mem.load32(kValue) == 2048u,
        "ATRAC frame sample count");
    check(atrac("sceAtracGetSoundSample", {id, kValue, kValue + 4u, kValue + 8u}) == 0u,
        "ATRAC sound sample query");
    check(mem.load32(kValue) == 2047u, "ATRAC end sample");
    check(mem.load32(kValue + 4u) == 0xFFFFFFFFu && mem.load32(kValue + 8u) == 0xFFFFFFFFu,
        "ATRAC reports no loop");
    constexpr std::uint32_t kPcm = Fixture::kOutput + 0x400u;
    check(atrac("sceAtracDecodeData", {id, kPcm, kValue, kValue + 4u, kValue + 8u}) == 0u,
        "ATRAC frame decodes");
    check(mem.load32(kValue) == 2048u, "ATRAC writes 2048 samples");
    check(mem.load32(kValue + 4u) == 1u, "ATRAC marks the stream end");
    check(mem.load32(kValue + 8u) == 0xFFFFFFFFu, "ATRAC reports in-memory data");
    check(mem.load16(kPcm) == 0u, "ATRAC terminator frame is silent");
    check(atrac("sceAtracDecodeData", {id, kPcm, kValue, kValue + 4u, kValue + 8u}) == 0x80630024u,
        "ATRAC reports all data decoded");
    check(mem.load32(kValue) == 0u && mem.load32(kValue + 4u) == 1u,
        "ATRAC end writes zero samples");
    check(atrac("sceAtracResetPlayPosition", {id, 0u, 0u, 0u}) == 0u, "ATRAC reset accepted");
    check(atrac("sceAtracDecodeData", {id, kPcm, kValue, kValue + 4u, kValue + 8u}) == 0u,
        "ATRAC decodes again after reset");
    check(atrac("sceAtracReleaseAtracID", {id}) == 0u, "ATRAC data ID released");

    const auto stream_file = atrac_riff(2u, 4096u);
    const auto stream_info = gungnir::audio::parse_atrac_riff(stream_file);
    check(stream_info.has_value() && stream_info->data_offset + 1536u < stream_file.size(),
        "stream fixture has a second frame");
    if (stream_info) {
        const std::uint32_t first_chunk = stream_info->data_offset + 1536u;
        for (std::size_t i = 0; i < stream_file.size(); ++i)
            mem.store8(kRiff + static_cast<std::uint32_t>(i), stream_file[i]);
        const std::uint32_t stream_id = atrac("sceAtracSetHalfwayBufferAndGetID",
            {kRiff, first_chunk, first_chunk + 1536u});
        check(stream_id > 0u, "streaming ATRAC opens");
        constexpr std::uint32_t kStreamInfo = Fixture::kOutput + 0x80u;
        check(atrac("sceAtracGetStreamDataInfo",
                  {stream_id, kStreamInfo, kStreamInfo + 4u, kStreamInfo + 8u}) == 0u,
            "stream data info available");
        const std::uint32_t write_pointer = mem.load32(kStreamInfo);
        check(write_pointer == kRiff, "stream write pointer is the guest buffer");
        check(mem.load32(kStreamInfo + 4u) == first_chunk + 1536u, "stream writable bytes");
        check(mem.load32(kStreamInfo + 8u) == first_chunk, "stream read offset");
        for (std::uint32_t i = 0u; i < 1536u; ++i)
            mem.store8(write_pointer + i, stream_file[first_chunk + i]);
        check(atrac("sceAtracAddStreamData", {stream_id, 1536u}) == 0u, "stream data appended");
        check(atrac("sceAtracGetRemainFrame", {stream_id, kStreamInfo}) == 0u &&
                  mem.load32(kStreamInfo) == 2u,
            "stream reports two remaining frames");
        check(atrac("sceAtracDecodeData", {stream_id, kPcm, kValue, kValue + 4u, kValue + 8u}) == 0u &&
                  mem.load32(kValue) == 2048u && mem.load32(kValue + 4u) == 0u,
            "first stream frame decodes");
        check(atrac("sceAtracDecodeData", {stream_id, kPcm, kValue, kValue + 4u, kValue + 8u}) == 0u &&
                  mem.load32(kValue) == 2048u && mem.load32(kValue + 4u) == 1u,
            "second stream frame ends the stream");
        check(atrac("sceAtracAddStreamData", {stream_id, first_chunk + 1536u}) == 0x80630018u,
            "oversized stream append is rejected");
        check(atrac("sceAtracReleaseAtracID", {stream_id}) == 0u, "streaming ID released");
    }
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


void savedata_contracts(const std::filesystem::path &memory_stick) {
    Fixture f(test_nids_path(), {}, memory_stick);
    auto &mem = f.runtime.memory();
    constexpr std::uint32_t kParam = 0x08830000u;
    constexpr std::uint32_t kMsFree = 0x08831000u;
    constexpr std::uint32_t kMsData = 0x08831100u;
    constexpr std::uint32_t kUtility = 0x08831200u;
    constexpr std::uint32_t kData = 0x08831300u;
    const auto zero = [&mem](std::uint32_t address, std::uint32_t size) {
        for (std::uint32_t i = 0; i < size; ++i) mem.store8(address + i, 0u);
    };
    const auto text = [&mem](std::uint32_t address, std::uint32_t offset, std::uint32_t size, std::string_view value) {
        for (std::uint32_t i = 0; i < size; ++i)
            mem.store8(address + offset + i, i < value.size() ? static_cast<std::uint8_t>(value[i]) : 0u);
    };
    const auto param = [&](std::string_view save_name) {
        zero(kParam, 0x600u);
        zero(kMsFree, 0x100u);
        zero(kMsData, 0x100u);
        zero(kUtility, 0x100u);
        mem.store32(kParam + 0x00u, 0x600u);
        mem.store32(kParam + 0x30u, 8u); // SIZES
        text(kParam, 0x3Cu, 13u, "ULUS10592");
        text(kParam, 0x4Cu, 20u, save_name);
        text(kParam, 0x64u, 13u, "SYSTEM");
        mem.store32(kParam + 0x74u, kData);
        mem.store32(kParam + 0x78u, 0x48u);
        mem.store32(kParam + 0x7Cu, 0x48u);
        mem.store32(kParam + 0x5D0u, kMsFree);
        mem.store32(kParam + 0x5D4u, kMsData);
        mem.store32(kParam + 0x5D8u, kUtility);
        text(kMsData, 0x00u, 13u, "ULUS10592");
        text(kMsData, 0x10u, 20u, save_name);
    };
    const auto util = [&f](const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return f.call("sceUtility", name, args);
    };

    const std::filesystem::path save_dir = memory_stick / "PSP" / "SAVEDATA" / "ULUS10592GUN9THSYS";
    std::filesystem::create_directories(save_dir);
    {
        std::ofstream system_file(save_dir / "SYSTEM", std::ios::binary);
        system_file << std::string(72u, 'x');
        std::ofstream sfo_file(save_dir / "PARAM.SFO", std::ios::binary);
        sfo_file << std::string(100u, 's');
        std::ofstream icon_file(save_dir / "ICON0.PNG", std::ios::binary);
        icon_file << std::string(2000u, 'i');
    }

    param("GUN9THSYS");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "sizes init accepted");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0x80110001u, "second init rejected while active");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "sizes update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "sizes reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0u, "sizes result is success");
    check(mem.load32(kMsFree + 0x04u) != 0u, "free clusters filled");
    check(mem.load32(kMsData + 0x24u) != 0u, "used clusters filled from the save directory");
    check(mem.load32(kUtility + 0x00u) != 0u, "utility used clusters filled");
    check(util("sceUtilitySavedataShutdownStart") == 0u, "sizes shutdown accepted");
    check(util("sceUtilitySavedataGetStatus") == 4u, "sizes reaches shutdown");

    gungnir::hle::reset_utility_state();
    param("MISSING");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "missing-save init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "missing-save update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "missing-save reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0x801103C7u, "missing save reports no data");
    check(util("sceUtilitySavedataShutdownStart") == 0u, "missing-save shutdown accepted");
    check(util("sceUtilitySavedataGetStatus") == 4u, "missing-save reaches shutdown");

    gungnir::hle::reset_utility_state();
    param("GUN9THSYS");
    mem.store32(kParam, 0x123u);
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0x80110004u, "bad param size rejected");

    gungnir::hle::reset_utility_state();
    param("..");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0x80110004u, "path traversal rejected");

    gungnir::hle::reset_utility_state();
    constexpr std::uint32_t kLoadBuf = 0x08831400u;
    const std::string payload = "savedata-round-trip";
    zero(kData, 0x100u);
    for (std::size_t i = 0; i < payload.size(); ++i)
        mem.store8(kData + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(payload[i]));

    const auto mode_param = [&](std::uint32_t mode, std::string_view save_name, std::uint32_t buffer,
                                std::uint32_t buffer_size, std::uint32_t data_size, std::string_view title) {
        zero(kParam, 0x600u);
        mem.store32(kParam + 0x00u, 0x600u);
        mem.store32(kParam + 0x30u, mode);
        text(kParam, 0x3Cu, 13u, "ULUS10592");
        text(kParam, 0x4Cu, 20u, save_name);
        text(kParam, 0x64u, 13u, "DATA.BIN");
        mem.store32(kParam + 0x74u, buffer);
        mem.store32(kParam + 0x78u, buffer_size);
        mem.store32(kParam + 0x7Cu, data_size);
        text(kParam, 0x80u, 0x80u, title);
        text(kParam, 0x100u, 0x80u, "Fixture Save");
        text(kParam, 0x180u, 0x400u, "Synthetic savedata detail");
        mem.store8(kParam + 0x580u, 2u);
    };
    const auto guest_text = [&mem](std::uint32_t address, std::uint32_t size) {
        std::string text;
        for (std::uint32_t i = 0; i < size; ++i) {
            const char value = static_cast<char>(mem.load8(address + i));
            if (value == '\0') break;
            text.push_back(value);
        }
        return text;
    };
    const auto read_host = [](const std::filesystem::path &path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    mode_param(3u, "MODES", kData, 0x100u, static_cast<std::uint32_t>(payload.size()), "Fixture Title");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "save init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "save update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "save reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0u, "save reports success");
    const std::filesystem::path modes_dir = memory_stick / "PSP" / "SAVEDATA" / "ULUS10592MODES";
    check(std::filesystem::is_regular_file(modes_dir / "PARAM.SFO"), "save writes PARAM.SFO");
    check(std::filesystem::is_regular_file(modes_dir / "DATA.BIN"), "save writes the data file");
    const auto sfo_before = read_host(modes_dir / "PARAM.SFO");
    const auto data_before = read_host(modes_dir / "DATA.BIN");
    check(data_before.size() == payload.size(), "save writes the requested data size");
    const auto sfo_fields = gungnir::savedata::parse_sfo_fields(sfo_before);
    check(sfo_fields.has_value(), "saved PARAM.SFO parses");
    check(sfo_fields->title == "Fixture Title", "saved PARAM.SFO records the title");
    check(sfo_fields->savedata_title == "Fixture Save", "saved PARAM.SFO records the savedata title");
    check(sfo_fields->detail == "Synthetic savedata detail", "saved PARAM.SFO records the detail");
    check(sfo_fields->parental_level == 2u, "saved PARAM.SFO records the parental level");

    gungnir::hle::reset_utility_state();
    zero(kLoadBuf, 0x100u);
    mode_param(2u, "MODES", kLoadBuf, 0x100u, 0u, "");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "load init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "load update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "load reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0u, "load reports success");
    check(mem.load32(kParam + 0x7Cu) == payload.size(), "load reports the file size");
    check(mem.load32(kParam + 0x34u) == 1021u, "load sets the bind value");
    check(guest_text(kParam + 0x80u, 0x80u) == "Fixture Title", "load restores the title");
    check(guest_text(kParam + 0x100u, 0x80u) == "Fixture Save", "load restores the savedata title");
    bool payload_matches = true;
    for (std::size_t i = 0; i < payload.size(); ++i)
        payload_matches = payload_matches && mem.load8(kLoadBuf + static_cast<std::uint32_t>(i)) ==
            static_cast<std::uint8_t>(payload[i]);
    check(payload_matches, "load restores the data bytes");

    gungnir::hle::reset_utility_state();
    mem.store32(kParam + 0x30u, 3u);
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "re-save init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "re-save update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "re-save reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0u, "re-save reports success");
    check(read_host(modes_dir / "PARAM.SFO") == sfo_before, "save after load reproduces PARAM.SFO");
    check(read_host(modes_dir / "DATA.BIN") == data_before, "save after load reproduces the data file");

    gungnir::hle::reset_utility_state();
    mode_param(2u, "MISSING2", kLoadBuf, 0x100u, 0u, "");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "missing load init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "missing load update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "missing load reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0x80110307u, "missing load reports no data");

    const std::filesystem::path broken_dir = memory_stick / "PSP" / "SAVEDATA" / "ULUS10592BROKEN";
    std::filesystem::create_directories(broken_dir);
    {
        std::ofstream sfo(broken_dir / "PARAM.SFO", std::ios::binary);
        sfo << "not-a-param-sfo";
    }
    gungnir::hle::reset_utility_state();
    mode_param(2u, "BROKEN", kLoadBuf, 0x100u, 0u, "");
    check(util("sceUtilitySavedataInitStart", {kParam}) == 0u, "broken load init accepted");
    check(util("sceUtilitySavedataUpdate", {0u}) == 0u, "broken load update accepted");
    check(util("sceUtilitySavedataGetStatus") == 3u, "broken load reaches finished");
    check(mem.load32(kParam + 0x1Cu) == 0x80110306u, "broken PARAM.SFO reports broken data");
}

void async_io_contracts(const std::filesystem::path &iso, const std::filesystem::path &memory_stick) {
    Fixture f(test_nids_path(), iso, memory_stick);
    auto &mem = f.runtime.memory();
    const auto io = [&f](const std::string &name, std::initializer_list<std::uint32_t> args = {}) {
        return f.call("IoFileMgrForUser", name, args);
    };
    constexpr std::uint32_t kResult = Fixture::kOutput + 0x200u;
    constexpr std::uint32_t kAsyncBusy = 0x80020329u;
    constexpr std::uint32_t kNoAsync = 0x8002032Au;
    constexpr std::uint32_t kKernelBadFd = 0x80020323u;

    f.write_string(Fixture::kText, "disc0:/PSP_GAME/PARAM.SFO");
    const std::uint32_t fd = io("sceIoOpenAsync", {Fixture::kText, 1u, 0u});
    check(fd > 2u, "async open returns a descriptor immediately");
    check(io("sceIoWaitAsync", {fd, kResult}) == 0u, "async open wait completes");
    check(mem.load32(kResult) == fd && mem.load32(kResult + 4u) == 0u,
          "async open result is the descriptor");

    check(io("sceIoReadAsync", {fd, Fixture::kOutput, 4u}) == 0u, "async read starts");
    check(io("sceIoPollAsync", {fd, kResult}) == 0u, "async poll completes the read");
    check(mem.load32(kResult) == 4u, "async read reports four bytes");
    check(mem.load32(Fixture::kOutput) == 0x46535000u, "async read copied the PARAM.SFO magic");

    check(io("sceIoLseekAsync", {fd, 0u, 4u, 0u, 0u}) == 0u, "async seek starts");
    check(io("sceIoWaitAsync", {fd, kResult}) == 0u, "async seek wait completes");
    check(mem.load32(kResult) == 4u && mem.load32(kResult + 4u) == 0u, "async seek result is the offset");

    check(io("sceIoReadAsync", {fd, Fixture::kOutput, 4u}) == 0u, "second async read starts");
    check(io("sceIoReadAsync", {fd, Fixture::kOutput, 4u}) == kAsyncBusy, "busy async op is rejected");
    check(io("sceIoWaitAsync", {fd, kResult}) == 0u, "busy read still completes");

    check(io("sceIoCloseAsync", {fd}) == 0u, "async close starts");
    check(io("sceIoWaitAsync", {fd, kResult}) == 0u, "async close wait completes");
    check(mem.load32(kResult) == 0u, "async close result is zero");
    check(io("sceIoWaitAsync", {fd, kResult}) == kKernelBadFd, "closed descriptor is rejected");

    f.write_string(Fixture::kText, "disc0:/PSP_GAME/missing.bin");
    const std::uint32_t missing = io("sceIoOpenAsync", {Fixture::kText, 1u, 0u});
    check(missing > 2u, "failed async open still returns a descriptor");
    check(io("sceIoWaitAsync", {missing, kResult}) == 0u, "failed open wait completes");
    check(mem.load32(kResult) == 0x80010002u, "failed open result is file-not-found");
    check(io("sceIoWaitAsync", {missing, kResult}) == kKernelBadFd, "failed descriptor is released");

    f.write_string(Fixture::kText, "ms0:/async-test.bin");
    const std::uint32_t sync_fd = io("sceIoOpen", {Fixture::kText, 0x0602u, 0u});
    check(io("sceIoPollAsync", {sync_fd, kResult}) == kNoAsync, "poll without async reports NOASYNC");
    check(io("sceIoWaitAsync", {sync_fd, kResult}) == kNoAsync, "wait without async reports NOASYNC");
    check(io("sceIoClose", {sync_fd}) == 0u, "sync descriptor closes");

    const std::uint32_t cb_fd = io("sceIoOpenAsync", {Fixture::kText, 0x0602u, 0u});
    check(cb_fd > 2u, "waitCB open returns a descriptor");
    check(io("sceIoWaitAsyncCB", {cb_fd, kResult}) == 0u, "waitCB completes");
    check(mem.load32(kResult) == cb_fd, "waitCB result is the descriptor");
    check(io("sceIoClose", {cb_fd}) == 0u, "waitCB descriptor closes");

    const std::uint32_t stat_fd = io("sceIoOpenAsync", {Fixture::kText, 0x0602u, 0u});
    check(io("sceIoGetAsyncStat", {stat_fd, 0u, kResult}) == 0u, "getAsyncStat waits and completes");
    check(mem.load32(kResult) == stat_fd, "getAsyncStat result is the descriptor");
    check(io("sceIoClose", {stat_fd}) == 0u, "stat descriptor closes");

    check(io("sceIoPollAsync", {0xFFFFu, kResult}) == kKernelBadFd, "invalid descriptor poll is rejected");
    check(io("sceIoReadAsync", {0xFFFFu, Fixture::kOutput, 4u}) == kKernelBadFd,
          "invalid descriptor read is rejected");

    const std::uint32_t seek_fd = io("sceIoOpenAsync", {Fixture::kText, 0x0602u, 0u});
    check(io("sceIoWaitAsync", {seek_fd, kResult}) == 0u, "seek fixture open completes");
    check(io("sceIoLseekAsync", {seek_fd, 0u, 0u, 0u, 9u}) == 0x80010016u, "bad whence is rejected");
    check(io("sceIoClose", {seek_fd}) == 0u, "seek fixture closes");
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
        audio_contracts();
        sas_contracts();
        atrac_contracts();
        lwmutex_contracts();
        if (!memory_stick.empty()) {
            std::filesystem::create_directories(memory_stick);
            if (!iso.empty()) {
                io_contracts(iso, memory_stick);
                async_io_contracts(iso, memory_stick);
            }
            savedata_contracts(memory_stick);
        }
        std::cout << "hle_kernel_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

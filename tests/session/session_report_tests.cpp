// T13a — shared session report contracts.
//
// Usage: session_report_tests
// Exit 0 on the asserted contract, 1 on a failed contract.

#include "session/diagnostics.hpp"
#include "session/session_report.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace {

using gungnir::session::Result;
using gungnir::session::StopCause;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

Result sample() {
    Result result;
    result.module = "9th";
    result.entry = 0x08972544u;
    result.gp = 0x089A0000u;
    result.image_end = 0x08980000u;
    result.functions = 6057u;
    result.dispatches = 200000u;
    result.dispatch_work = 250000u;
    result.vblanks = 2178u;
    result.virtual_us = 36336654u;
    result.reached_first_frame_buf = true;
    result.flips = 738u;
    result.first_frame_buf = 0x04000000u;
    result.display_width = 480u;
    result.display_format = 3u;
    result.module_init_complete = true;
    result.stop_cause = StopCause::FirstFlip;
    result.threads = "user_main,Sound";
    result.hle_bound = 158u;
    result.imports_total = 215u;
    result.imports_bound = 158u;
    result.imports_unbound = {"sceMpeg:0x00000001"};
    result.imports_executed = {"sceDisplaySetFrameBuf"};
    result.stopped = true;
    result.stop_reason = "first sceDisplaySetFrameBuf 0x04000000";
    result.stop_pc = 0x0899C3DCu;
    return result;
}

} // namespace

int main() {
    check(gungnir::session::stop_cause_name(StopCause::FirstFlip) == std::string("first_flip"),
        "first_flip name");
    check(gungnir::session::stop_cause_name(StopCause::MissingImport) == std::string("missing_import"),
        "missing_import name");
    check(gungnir::session::stop_cause_name(StopCause::DispatchLimit) == std::string("dispatch_limit"),
        "dispatch_limit name");
    check(gungnir::session::stop_cause_name(StopCause::WallClock) == std::string("wall_clock"),
        "wall_clock name");
    check(gungnir::session::stop_cause_name(StopCause::SchedulerDeadlock) == std::string("scheduler_deadlock"),
        "scheduler_deadlock name");
    check(gungnir::session::stop_cause_name(StopCause::HostStop) == std::string("host_stop"),
        "host_stop name");
    check(gungnir::session::stop_cause_name(StopCause::GuestStop) == std::string("guest_stop"),
        "guest_stop name");

    check(gungnir::session::classify_stop_cause("first sceDisplaySetFrameBuf 0x04000000") ==
              StopCause::FirstFlip,
        "classify first flip");
    check(gungnir::session::classify_stop_cause(
              "Missing HLE import TestLibrary::sceNope (NID_0xDEADBEEF)") == StopCause::MissingImport,
        "classify missing import");
    check(gungnir::session::classify_stop_cause("Dispatch limit reached") == StopCause::DispatchLimit,
        "classify dispatch limit");
    check(gungnir::session::classify_stop_cause("wall-clock limit reached") == StopCause::WallClock,
        "classify wall clock");
    check(gungnir::session::classify_stop_cause("scheduler deadlock") == StopCause::SchedulerDeadlock,
        "classify scheduler deadlock");
    check(gungnir::session::classify_stop_cause("host stop requested") == StopCause::HostStop,
        "classify host stop");
    check(gungnir::session::classify_stop_cause("guest exited") == StopCause::GuestStop,
        "classify guest stop");
    check(gungnir::session::classify_stop_cause("") == StopCause::GuestStop,
        "empty reason is guest stop");

    const Result result = sample();
    const std::string summary = gungnir::session::summary_line(result);
    const std::string expected =
        "session_stop cause=first_flip dispatches=200000 flips=738 vblanks=2178 "
        "imports_bound=158 imports_total=215 imports_unbound=1 imports_executed=1 "
        "missing_import=0 silent_nids=0 module_init_complete=true stop_pc=0x0899C3DC";
    check(summary == expected, "summary line: " + summary);

    const std::string json = gungnir::session::result_json(result);
    for (const std::string &needle : {"\"module\": \"9th\"", "\"stop_cause\": \"first_flip\"",
             "\"reached_first_frame_buf\": true", "\"imports_unbound\": [\"sceMpeg:0x00000001\"]",
             "\"stop_pc\": \"0x0899C3DC\""})
        check(json.find(needle) != std::string::npos, "json missing " + needle);

    Result missing = result;
    missing.stop_cause = StopCause::MissingImport;
    missing.stop_reason = "Missing HLE import Lib::Nope (NID_0xDEADBEEF)";
    missing.module_init_complete = false;
    check(gungnir::session::summary_line(missing).find("missing_import=1") != std::string::npos,
        "summary reports a missing import");
    check(gungnir::session::summary_line(missing).find("module_init_complete=false") !=
              std::string::npos,
        "summary reports the cleared module-init gate");

    gungnir::session::configure_chain_ring(4u);
    gungnir::session::chain_ring_enter(0x1111u, 0u, 0xAAAAu, 1u, 2u, 3u, 4u, "user_main");
    gungnir::session::chain_ring_leave(0x1111u, 0u, 0x99u, 0x88u);
    std::ostringstream chain;
    gungnir::session::dump_chain_ring(chain);
    check(chain.str().find("target=0x00001111") != std::string::npos,
        "chain ring records the target");
    check(chain.str().find("thread=user_main") != std::string::npos,
        "chain ring records the thread");
    check(chain.str().find("ra=0x0000AAAA") != std::string::npos,
        "chain ring records the return PC");
    check(chain.str().find("v0=0x00000099") != std::string::npos,
        "chain ring records the return value");

    gungnir::session::chain_ring_enter(0x2222u, 0u, 0xBBBBu, 0u, 0u, 0u, 0u, "user_main");
    gungnir::session::chain_ring_enter(0x3333u, 1u, 0xCCCCu, 0u, 0u, 0u, 0u, "user_main");
    std::ostringstream nested;
    gungnir::session::dump_chain_ring(nested);
    check(nested.str().find("target=0x00003333") != std::string::npos &&
            nested.str().find("inflight=1") != std::string::npos,
        "chain ring reports in-flight frames");
    gungnir::session::chain_ring_leave(0x3333u, 1u, 0u, 0u);
    gungnir::session::chain_ring_leave(0x2222u, 0u, 0u, 0u);

    gungnir::session::configure_chain_ring(2u);
    for (std::uint32_t target : {0x4444u, 0x5555u, 0x6666u}) {
        gungnir::session::chain_ring_enter(target, 0u, 0u, 0u, 0u, 0u, 0u, "user_main");
        gungnir::session::chain_ring_leave(target, 0u, 0u, 0u);
    }
    std::ostringstream capped;
    gungnir::session::dump_chain_ring(capped);
    check(capped.str().find("target=0x00004444") == std::string::npos,
        "chain ring drops the oldest entry at capacity");
    check(capped.str().find("target=0x00006666") != std::string::npos,
        "chain ring keeps the newest entry at capacity");

    gungnir::session::configure_chain_ring(0u);
    std::ostringstream disabled;
    gungnir::session::dump_chain_ring(disabled);
    check(disabled.str().empty(), "disabled chain ring dumps nothing");

    if (failures != 0) {
        std::cerr << "session_report_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "session_report_tests: OK\n";
    return 0;
}

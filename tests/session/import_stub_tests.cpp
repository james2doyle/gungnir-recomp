// Interpreter-only import dispatch regression (k9ll558m, red-first).
//
// Guest code in memory:
//
//   kCode+0:  jal stub
//   kCode+4:  nop                    (delay slot)
//   kCode+8:  addiu v0, v0, 1
//   kCode+12: jal kExit
//   kCode+16: nop                    (delay slot)
//
//   kStub:    jr ra; nop              (a tail-stub import, as in real modules)
//   kExit:    host stop function
//
// Without a registered function at the stub, the interpreter executes the
// tail stub and returns without calling the HLE (the module_start no-op
// bug). With register_import_stub_trampolines, the HLE runs and execution
// continues after the call. A pre-registered function at the stub must
// survive registration untouched (the AOT build case).
//
// Exit 0 on the asserted contract, 1 on a failed contract.

#include "session/import_stub.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/guest_memory.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using psprecomp::AllegrexContext;
using psprecomp::Runtime;

constexpr std::uint32_t kCode = 0x08804000u;
constexpr std::uint32_t kStub = 0x08804100u;
constexpr std::uint32_t kExit = 0x08804200u;
constexpr const char *kLibraryName = "TestLibrary";
constexpr std::uint32_t kNid = 0xDEADBEEFu;

constexpr std::uint32_t kNop = 0x00000000u;
constexpr std::uint32_t kJrRa = 0x03E00008u;
constexpr std::uint32_t kAddiuV0 = 0x24420001u; // addiu v0, v0, 1

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

std::uint32_t jal_word(std::uint32_t target) {
    return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu);
}

void plant_guest(Runtime &runtime) {
    psprecomp::GuestMemory &memory = runtime.memory();
    memory.store32(kCode, jal_word(kStub));
    memory.store32(kCode + 4u, kNop);
    memory.store32(kCode + 8u, kAddiuV0);
    memory.store32(kCode + 12u, jal_word(kExit));
    memory.store32(kCode + 16u, kNop);
    memory.store32(kStub, kJrRa);
    memory.store32(kStub + 4u, kNop);
}

void stop_at_exit(Runtime &rt, AllegrexContext &) {
    rt.stop("test guest exit");
}

int g_original_calls = 0;

void original_stub_function(Runtime &, AllegrexContext &ctx) {
    ++g_original_calls;
    ctx.pc = ctx.gpr[31];
}

void run_guest(Runtime &runtime) {
    runtime.cpu().gpr[31] = kExit;
    runtime.run(kCode, 64u);
}

// Red control: no trampoline. The interpreter runs the tail stub, so the
// HLE is never reached, but the guest still continues past the call.
void interpreter_without_trampoline_skips_the_import() {
    Runtime runtime;
    int calls = 0;
    runtime.register_hle(kLibraryName, kNid,
        [&calls](Runtime &, AllegrexContext &ctx) {
            ++calls;
            ctx.gpr[2] = 0x2Au;
        });
    runtime.register_function(kExit, &stop_at_exit, "test_exit");
    plant_guest(runtime);
    run_guest(runtime);
    check(calls == 0, "without a trampoline the import is never dispatched");
    check(runtime.cpu().gpr[2] == 1u,
          "without a trampoline the guest returns from the tail stub and continues");
}

// Green: trampoline registered. The HLE runs and the guest continues.
void interpreter_with_trampoline_dispatches_the_import() {
    Runtime runtime;
    int calls = 0;
    runtime.register_hle(kLibraryName, kNid,
        [&calls](Runtime &, AllegrexContext &ctx) {
            ++calls;
            ctx.gpr[2] = 0x2Au;
        });
    runtime.register_function(kExit, &stop_at_exit, "test_exit");
    plant_guest(runtime);
    const std::vector<psprecomp::PspImport> imports{
        psprecomp::PspImport{kLibraryName, kNid, kStub}};
    gungnir::session::register_import_stub_trampolines(runtime, imports);
    run_guest(runtime);
    check(calls == 1, "with a trampoline the HLE import runs exactly once");
    check(runtime.cpu().gpr[2] == 0x2Bu,
          "with a trampoline the guest continues after the import returns");
}

// A pre-registered (AOT) function at the stub must survive registration.
void pre_registered_function_is_not_overwritten() {
    Runtime runtime;
    int calls = 0;
    g_original_calls = 0;
    runtime.register_hle(kLibraryName, kNid,
        [&calls](Runtime &, AllegrexContext &ctx) {
            ++calls;
            ctx.gpr[2] = 0x2Au;
        });
    runtime.register_function(kExit, &stop_at_exit, "test_exit");
    runtime.register_function(kStub, &original_stub_function, "test_original");
    plant_guest(runtime);
    const std::vector<psprecomp::PspImport> imports{
        psprecomp::PspImport{kLibraryName, kNid, kStub}};
    gungnir::session::register_import_stub_trampolines(runtime, imports);
    run_guest(runtime);
    check(g_original_calls == 1, "pre-registered stub function still runs");
    check(calls == 0, "trampoline does not displace a pre-registered function");
    check(runtime.cpu().gpr[2] == 1u,
          "guest continues after the pre-registered stub returns");
}

} // namespace

int main() {
    try {
        interpreter_without_trampoline_skips_the_import();
        interpreter_with_trampoline_dispatches_the_import();
        pre_registered_function_is_not_overwritten();
        std::cout << "import_stub_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

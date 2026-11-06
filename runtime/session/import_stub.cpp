#include "session/import_stub.hpp"

#include "psprecomp/common.hpp"

#include <string>
#include <unordered_map>

namespace gungnir::session {

namespace {

struct ImportStubBinding {
    std::uint32_t slot{};
    std::string library;
    std::uint32_t nid{};
};

// One active session at a time (same discipline as the session's g_active);
// register_import_stub_trampolines replaces the map on every start.
std::unordered_map<std::uint32_t, ImportStubBinding> g_import_stubs;

void import_stub_trampoline(psprecomp::Runtime &rt, psprecomp::AllegrexContext &ctx) {
    const std::uint32_t import_pc = ctx.pc;
    const std::uint32_t return_address = ctx.gpr[31];
    const auto caller_context = psprecomp::capture_runtime_execution_context();
    const auto found = g_import_stubs.find(import_pc);
    if (found == g_import_stubs.end()) {
        rt.stop("Interpreter import stub with no binding at " + psprecomp::hex32(import_pc));
        return;
    }
    const ImportStubBinding binding = found->second;
    rt.invoke_import_cached(binding.slot, binding.library, binding.nid, ctx);
    if (!rt.stopped() && psprecomp::runtime_execution_context_matches(caller_context) &&
        ctx.pc == import_pc) ctx.pc = return_address;
}

} // namespace

void register_import_stub_trampolines(psprecomp::Runtime &runtime,
    const std::vector<psprecomp::PspImport> &imports) {
    g_import_stubs.clear();
    std::uint32_t slot = 0u;
    for (const psprecomp::PspImport &import : imports) {
        if (!runtime.has_function(import.stub_address)) {
            g_import_stubs.emplace(import.stub_address,
                ImportStubBinding{slot, import.library, import.nid});
            runtime.register_function(import.stub_address, &import_stub_trampoline,
                import.library + "::import_stub");
        }
        ++slot;
    }
}

} // namespace gungnir::session

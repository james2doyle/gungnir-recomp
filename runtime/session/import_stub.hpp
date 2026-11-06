#pragma once

// Interpreter-only import dispatch (k9ll558m): the release APK ships no
// generated AOT corpus, so no AOT import thunks are registered. Register the
// same targets differently: every scanned import stub without a registered
// function gets the shared trampoline below, which dispatches by stub
// address into the HLE. Without this the interpreter executes the stub code
// itself (jr $ra tail stubs) and the guest returns from module_start having
// done no work.

#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"

#include <vector>

namespace gungnir::session {

void register_import_stub_trampolines(psprecomp::Runtime &runtime,
    const std::vector<psprecomp::PspImport> &imports);

} // namespace gungnir::session

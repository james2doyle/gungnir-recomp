// T7a contract tests: GE display-list decoding and bounded execution.
//
// Every word in these fixtures is hand-authored for this test; no game data is
// read. Expected addresses and vertex layouts are computed by hand below.

#include "ge/display_list.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace gungnir::ge;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

constexpr std::uint32_t word(std::uint32_t opcode, std::uint32_t argument) {
    return (opcode << 24u) | (argument & 0xFFFFFFu);
}

class Words {
public:
    void put(std::uint32_t address, std::uint32_t value) { cells[address] = value; }

    std::optional<std::uint32_t> read(std::uint32_t address) const {
        const auto found = cells.find(address);
        if (found == cells.end()) return std::nullopt;
        return found->second;
    }

    WordReader reader() const {
        return [this](std::uint32_t address) { return read(address); };
    }

    std::map<std::uint32_t, std::uint32_t> cells;
};

void word_decoding() {
    check(decode_word_opcode(0x04ABCDEFu) == 0x04u, "opcode occupies bits 24-31");
    check(decode_word_argument(0x04ABCDEFu) == 0x00ABCDEFu, "argument occupies bits 0-23");
    check(std::string(opcode_name(0x04)) == "PRIM", "PRIM is named");
    check(is_defined_opcode(0x04u), "PRIM is defined");
    check(is_defined_opcode(0x0Eu), "SIGNAL is defined");
    check(is_defined_opcode(0xD3u), "CLEARMODE is defined");
    check(is_defined_opcode(0xFFu), "NOP_FF is defined");
    check(!is_defined_opcode(0x0Du), "0x0D is undefined");
    check(std::string(opcode_name(0x0D)) == "UNKNOWN", "undefined opcode has no name");
}

void vertex_layouts() {
    const VertexLayout mixed = decode_vertex_layout(0x000491BEu);
    check(mixed.texcoord == 2u, "texcoord is 16-bit");
    check(mixed.color == 7u, "color is 8888");
    check(mixed.normal == 1u, "normal is 8-bit");
    check(mixed.position == 3u, "position is float");
    check(mixed.weight == 0u, "no weights");
    check(mixed.index == 2u, "indices are 16-bit");
    check(mixed.weight_count == 3u, "three weights");
    check(mixed.morph_count == 2u, "two morph targets");
    check(!mixed.through, "not through-mode");
    check(mixed.texcoord_offset == 0u, "texcoord starts at 0");
    check(mixed.color_offset == 4u, "color starts after 2x16-bit texcoord");
    check(mixed.normal_offset == 8u, "normal starts after 8888 color");
    check(mixed.position_offset == 12u, "position aligns to 4");
    check(mixed.weight_offset == 0u, "no weight offset");
    check(mixed.stride == 48u, "24-byte target times two morphs");
    check(mixed.index_size == 2u, "16-bit index size");

    const VertexLayout weighted = decode_vertex_layout(0x0000C300u);
    check(weighted.weight == 1u && weighted.weight_count == 4u, "four 8-bit weights");
    check(weighted.weight_offset == 0u, "weights lead the vertex");
    check(weighted.position == 2u, "position is 16-bit");
    check(weighted.position_offset == 4u, "position aligns to 2 after four weights");
    check(weighted.stride == 10u, "four weights plus three 16-bit position components");
    check(weighted.index_size == 0u, "no index");
}

void address_rules() {
    Words words;
    words.put(0x1000u, word(0x10u, 0x00080000u));
    words.put(0x1004u, word(0x13u, 0x00000010u));
    words.put(0x1008u, word(0x01u, 0x00000020u));
    words.put(0x100Cu, word(0x02u, 0x00000024u));
    words.put(0x1010u, word(0x14u, 0x00000000u));
    words.put(0x1014u, word(0x01u, 0x00000020u));
    words.put(0x1018u, word(0x0Cu, 0x00000000u));

    DisplayList list(words.reader(), {.record_trace = true});
    list.reset(0x1000u);
    const RunResult result = list.run();
    check(result.status == RunStatus::Completed, "address list completes");
    check(list.trace().size() == 7u, "seven address commands decoded");

    check(list.trace()[0].kind == CommandKind::Base && list.trace()[0].base == 0x08000000u,
          "BASE takes high byte from bits 16-23");
    check(list.trace()[1].kind == CommandKind::Offset && list.trace()[1].offset == 0x00001000u,
          "OFFSET shifts its argument by eight");
    check(list.trace()[2].address == 0x08001020u, "VADDR adds base and offset");
    check(list.trace()[3].address == 0x08001024u, "IADDR adds base and offset");
    check(list.trace()[4].kind == CommandKind::Origin && list.trace()[4].offset == 0x1010u,
          "ORIGIN captures its own address");
    check(list.trace()[5].address == 0x08001030u, "VADDR uses the new origin");
    check(list.base() == 0x08000000u && list.offset() == 0x1010u, "runner keeps base and offset");
    check(list.trace()[6].kind == CommandKind::End, "END is typed");
}

void jump_call_return() {
    Words words;
    words.put(0x2000u, word(0x0Au, 0x00002040u));
    words.put(0x2004u, word(0x00u, 0x00000000u));
    words.put(0x2008u, word(0x0Cu, 0x00000000u));
    words.put(0x2040u, word(0x0Au, 0x00002060u));
    words.put(0x2044u, word(0x00u, 0x00000000u));
    words.put(0x2048u, word(0x0Bu, 0x00000000u));
    words.put(0x2060u, word(0x00u, 0x00000000u));
    words.put(0x2064u, word(0x0Bu, 0x00000000u));

    DisplayList list(words.reader(), {.record_trace = true});
    list.reset(0x2000u);
    const RunResult result = list.run();
    check(result.status == RunStatus::Completed, "nested call list completes");
    check(list.trace().size() == 8u, "nested call executes both frames");
    const std::uint32_t expected[] = {0x2000u, 0x2040u, 0x2060u, 0x2064u, 0x2044u, 0x2048u, 0x2004u, 0x2008u};
    for (std::size_t i = 0; i < 8u; ++i) check(list.trace()[i].pc == expected[i], "call/return PC order");
    check(list.trace()[0].address == 0x2040u, "CALL resolves its target");
    check(list.trace()[3].address == 0x2044u, "inner RET returns to the inner call site");
    check(list.trace()[5].address == 0x2004u, "outer RET returns to the outer call site");
    check(list.call_depth() == 0u, "call stack unwinds");

    Words jump;
    jump.put(0x3000u, word(0x08u, 0x00003010u));
    jump.put(0x3010u, word(0x0Cu, 0x00000000u));
    DisplayList jumped(jump.reader(), {.record_trace = true});
    jumped.reset(0x3000u);
    check(jumped.run().status == RunStatus::Completed, "JUMP list completes");
    check(jumped.trace().size() == 2u && jumped.trace()[0].address == 0x3010u, "JUMP skips the fallthrough");

    Words empty_return;
    empty_return.put(0x4000u, word(0x0Bu, 0x00000000u));
    DisplayList returned(empty_return.reader(), {.record_trace = true});
    returned.reset(0x4000u);
    const RunResult bad_return = returned.run();
    check(bad_return.status == RunStatus::ReturnWithoutCall, "RET without CALL fails loudly");
    check(bad_return.pc == 0x4000u && std::string(run_status_name(bad_return.status)) == "return_without_call",
          "RET error names its contract");
    check(returned.trace().size() == 1u && returned.trace()[0].kind == CommandKind::Return, "failed RET is traced");

    Words deep;
    deep.put(0x5000u, word(0x0Au, 0x00005020u));
    deep.put(0x5020u, word(0x0Au, 0x00005040u));
    deep.put(0x5040u, word(0x0Au, 0x00005060u));
    deep.put(0x5060u, word(0x0Bu, 0x00000000u));
    DisplayList nested(deep.reader(), {.record_trace = true});
    nested.reset(0x5000u);
    const RunResult too_deep = nested.run();
    check(too_deep.status == RunStatus::CallDepthExceeded, "third nested CALL exceeds the two-level stack");
    check(too_deep.pc == 0x5040u && std::string(run_status_name(too_deep.status)) == "call_depth_exceeded",
          "call depth error names its contract");
    check(nested.trace().size() == 3u, "depth failure happens at the third CALL");
}

void stall_and_resume() {
    Words words;
    words.put(0x6000u, word(0x00u, 0x00000000u));
    words.put(0x6004u, word(0x00u, 0x00000000u));
    words.put(0x6008u, word(0x00u, 0x00000000u));
    words.put(0x600Cu, word(0x0Cu, 0x00000000u));

    DisplayList list(words.reader(), {.record_trace = true});
    list.reset(0x6000u, 0x6008u);
    const RunResult stopped = list.run();
    check(stopped.status == RunStatus::Stalled, "stall address stops the list");
    check(stopped.pc == 0x6008u && list.pc() == 0x6008u, "stall stops before the target command");
    check(list.executed() == 2u && list.trace().size() == 2u, "commands before the stall executed");
    check(list.stalled() && !list.completed(), "stalled list is resumable");
    check(stopped.ok(), "stall is not an error");

    list.set_stall(0u);
    const RunResult finished = list.run();
    check(finished.status == RunStatus::Completed, "clearing the stall resumes to END");
    check(list.executed() == 4u && list.trace().size() == 4u, "resume does not re-execute");
    check(list.completed() && !list.stalled(), "resumed list is complete");
    check(list.trace()[3].kind == CommandKind::End, "resume reaches END");
}

void signal_finish_and_end() {
    Words words;
    words.put(0x7000u, word(0x0Eu, 0x00010002u));
    words.put(0x7004u, word(0x0Fu, 0x00000003u));
    words.put(0x7008u, word(0x0Cu, 0x00000000u));

    DisplayList list(words.reader(), {.record_trace = true});
    list.reset(0x7000u);
    check(list.run().status == RunStatus::Completed, "signal/finish list completes at END");
    check(list.trace().size() == 3u, "signal, finish and end are decoded");
    check(list.trace()[0].kind == CommandKind::Signal && list.trace()[0].argument == 0x00010002u,
          "SIGNAL keeps its argument");
    check(list.trace()[1].kind == CommandKind::Finish && list.trace()[1].argument == 0x00000003u,
          "FINISH keeps its argument");
    check(list.trace()[2].kind == CommandKind::End, "END terminates");
}

void branch_and_state_commands() {
    Words words;
    words.put(0xD000u, word(0x07u, 0x00000000u));
    words.put(0xD004u, word(0x09u, 0x0000D100u));
    words.put(0xD008u, word(0xD3u, 0x00000001u));
    words.put(0xD00Cu, word(0xFFu, 0x00000000u));
    words.put(0xD010u, word(0x0Cu, 0x00000000u));
    words.put(0xD100u, word(0x0Cu, 0x00000000u));

    DisplayList list(words.reader(), {.record_trace = true});
    list.reset(0xD000u);
    check(list.run().status == RunStatus::Completed, "branch/state list completes");
    check(list.trace().size() == 5u, "branch and state commands are traced");
    check(list.trace()[0].kind == CommandKind::BoundingBox, "BOUNDINGBOX is typed");
    check(list.trace()[1].kind == CommandKind::BranchJump && !list.trace()[1].branch_taken,
          "BJUMP falls through while the bounding box is assumed visible");
    check(list.trace()[2].kind == CommandKind::State && list.trace()[2].opcode == 0xD3u,
          "CLEARMODE is a typed state command");
    check(list.trace()[3].kind == CommandKind::Nop && list.trace()[3].opcode == 0xFFu,
          "0xFF is NOP_FF, not an unknown opcode");
    check(std::string(opcode_name(0xD3u)) == "CLEARMODE", "state command keeps its name");
}

void unknown_opcode_and_invalid_primitive() {
    Words unknown;
    unknown.put(0x8000u, word(0x0Du, 0x00ABCDEFu));
    DisplayList list(unknown.reader(), {.record_trace = true});
    list.reset(0x8000u);
    const RunResult failed = list.run();
    check(failed.status == RunStatus::UnknownOpcode, "undefined opcode traps");
    check(failed.pc == 0x8000u && failed.word == word(0x0Du, 0x00ABCDEFu), "unknown opcode reports its word");
    check(std::string(run_status_name(failed.status)) == "unknown_opcode", "unknown opcode is named");
    check(!failed.ok(), "unknown opcode is an error");
    check(list.trace().size() == 1u && list.trace()[0].kind == CommandKind::Unknown, "unknown opcode is traced");
    const RunResult repeated = list.run();
    check(repeated.status == RunStatus::UnknownOpcode && list.executed() == 1u,
          "a failed list does not execute again");

    Words invalid;
    invalid.put(0x9000u, word(0x04u, 0x00070003u));
    DisplayList prim(invalid.reader(), {.record_trace = true});
    prim.reset(0x9000u);
    const RunResult bad_prim = prim.run();
    check(bad_prim.status == RunStatus::InvalidPrimitive, "primitive type 7 traps");
    check(std::string(run_status_name(bad_prim.status)) == "invalid_primitive", "invalid primitive is named");
}

void command_limit_and_memory_fault() {
    Words loop;
    loop.put(0xA000u, word(0x08u, 0x0000A000u));
    DisplayList list(loop.reader(), {.max_commands = 8u, .record_trace = true});
    list.reset(0xA000u);
    const RunResult bounded = list.run();
    check(bounded.status == RunStatus::CommandLimit, "self-jump hits the command cap");
    check(bounded.pc == 0xA000u && list.executed() == 8u && list.trace().size() == 8u,
          "command cap counts executed words");
    check(std::string(run_status_name(bounded.status)) == "command_limit", "command cap is named");

    DisplayList missing([](std::uint32_t) { return std::optional<std::uint32_t>{}; });
    missing.reset(0xB000u);
    const RunResult fault = missing.run();
    check(fault.status == RunStatus::MemoryFault && fault.pc == 0xB000u, "unreadable word traps");
    check(std::string(run_status_name(fault.status)) == "memory_fault", "memory fault is named");
    check(missing.trace().empty(), "memory fault records no command");
}

void deterministic_trace() {
    Words words;
    words.put(0xC000u, word(0x10u, 0x00000000u));
    words.put(0xC004u, word(0x12u, 0x000491BEu));
    words.put(0xC008u, word(0x01u, 0x00000040u));
    words.put(0xC00Cu, word(0x04u, 0x00030006u));
    words.put(0xC010u, word(0x0Au, 0x0000C030u));
    words.put(0xC014u, word(0x0Cu, 0x00000000u));
    words.put(0xC030u, word(0x00u, 0x00000000u));
    words.put(0xC034u, word(0x0Bu, 0x00000000u));

    DisplayList first(words.reader(), {.record_trace = true});
    DisplayList second(words.reader(), {.record_trace = true});
    first.reset(0xC000u);
    second.reset(0xC000u);
    check(first.run().status == RunStatus::Completed, "trace list completes");
    check(second.run().status == RunStatus::Completed, "trace list completes twice");
    check(first.trace() == second.trace(), "same words produce the same command trace");
    const std::string a = format_trace(first);
    const std::string b = format_trace(second);
    check(a == b, "formatted traces are deterministic");
    check(a.find("PRIM triangles x6") != std::string::npos, "trace names the primitive and count");
    check(a.find("VTYPE") != std::string::npos, "trace names the vertex type");
    check(a.find("CALL") != std::string::npos && a.find("RET") != std::string::npos, "trace records control flow");
}

} // namespace

int main() {
    try {
        word_decoding();
        vertex_layouts();
        address_rules();
        jump_call_return();
        stall_and_resume();
        signal_finish_and_end();
        branch_and_state_commands();
        unknown_opcode_and_invalid_primitive();
        command_limit_and_memory_fault();
        deterministic_trace();
        std::cout << "ge_display_list_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

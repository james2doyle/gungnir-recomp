#pragma once

// GE display-list command stream (T7a).
//
// The command encoding, base/offset address arithmetic and call/return rules
// follow the public GE command set as implemented by PPSSPP, KanaStation and
// psprecore (MIT). This decoder is standalone: it reads guest words through a
// callback and has no renderer or Vulkan dependency.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gungnir::ge {

enum class Primitive : std::uint8_t {
    Points = 0,
    Lines = 1,
    LineStrip = 2,
    Triangles = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprites = 6,
    Invalid = 7,
};

const char *primitive_name(Primitive primitive);

struct VertexLayout {
    std::uint32_t raw{};
    std::uint8_t texcoord{};
    std::uint8_t color{};
    std::uint8_t normal{};
    std::uint8_t position{};
    std::uint8_t weight{};
    std::uint8_t index{};
    std::uint8_t weight_count{1};
    std::uint8_t morph_count{1};
    bool through{};
    std::uint16_t texcoord_offset{};
    std::uint16_t color_offset{};
    std::uint16_t normal_offset{};
    std::uint16_t position_offset{};
    std::uint16_t weight_offset{};
    std::uint16_t stride{};
    std::uint8_t index_size{};

    bool operator==(const VertexLayout &) const = default;
};

VertexLayout decode_vertex_layout(std::uint32_t vertex_type);

enum class CommandKind : std::uint8_t {
    Nop,
    VertexAddress,
    IndexAddress,
    Primitive,
    Bezier,
    Spline,
    BoundingBox,
    Jump,
    BranchJump,
    Call,
    Return,
    End,
    Signal,
    Finish,
    Base,
    VertexType,
    Offset,
    Origin,
    State,
    Unknown,
};

const char *kind_name(CommandKind kind);

struct Command {
    CommandKind kind{};
    std::uint8_t opcode{};
    std::uint32_t pc{};
    std::uint32_t word{};
    std::uint32_t argument{};
    std::uint32_t address{};
    std::uint32_t base{};
    std::uint32_t offset{};
    Primitive primitive{};
    std::uint16_t count{};
    VertexLayout vertex_layout{};
    bool branch_taken{};

    bool operator==(const Command &) const = default;
};

std::uint32_t decode_word_opcode(std::uint32_t word);
std::uint32_t decode_word_argument(std::uint32_t word);
bool is_defined_opcode(std::uint8_t opcode);
const char *opcode_name(std::uint8_t opcode);
Command decode_command(std::uint32_t pc, std::uint32_t word);

enum class RunStatus : std::uint8_t {
    Completed,
    Stalled,
    UnknownOpcode,
    InvalidPrimitive,
    CommandLimit,
    CallDepthExceeded,
    ReturnWithoutCall,
    MemoryFault,
};

const char *run_status_name(RunStatus status);

struct RunResult {
    RunStatus status{RunStatus::Completed};
    std::uint32_t pc{};
    std::uint32_t word{};

    [[nodiscard]] bool ok() const {
        return status == RunStatus::Completed || status == RunStatus::Stalled;
    }
};

using WordReader = std::function<std::optional<std::uint32_t>(std::uint32_t)>;

class DisplayList {
public:
    struct Config {
        std::uint32_t max_commands{1'000'000u};
        bool record_trace{false};
        std::function<void(const Command &)> on_command;
    };

    DisplayList() = default;
    explicit DisplayList(WordReader reader);
    DisplayList(WordReader reader, Config config);

    void reset(std::uint32_t start, std::uint32_t stall = 0u);
    void set_stall(std::uint32_t stall);
    RunResult run();

    [[nodiscard]] bool completed() const { return done_; }
    [[nodiscard]] bool stalled() const { return status_ == RunStatus::Stalled && !done_; }
    [[nodiscard]] std::uint32_t pc() const { return pc_; }
    [[nodiscard]] std::uint32_t stall() const { return stall_; }
    [[nodiscard]] std::uint32_t base() const { return base_; }
    [[nodiscard]] std::uint32_t offset() const { return offset_; }
    [[nodiscard]] std::size_t call_depth() const { return call_stack_.size(); }
    [[nodiscard]] std::uint64_t executed() const { return executed_; }
    // Snapshot support (T15c): the call stack is guest control flow, so it is
    // saved verbatim as (return_pc, offset) pairs.
    [[nodiscard]] std::vector<std::pair<std::uint32_t, std::uint32_t>> call_stack() const;
    void restore_execution(std::uint32_t pc, std::uint32_t stall, std::uint32_t base, std::uint32_t offset,
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> &calls, std::uint64_t executed, bool done);
    [[nodiscard]] const std::vector<Command> &trace() const { return trace_; }

private:
    struct CallFrame {
        std::uint32_t return_pc{};
        std::uint32_t offset{};
    };

    std::uint32_t resolve(std::uint32_t argument) const;
    RunResult finish(RunStatus status, std::uint32_t word);
    void record(const Command &command);

    WordReader reader_;
    Config config_;
    std::uint32_t pc_{};
    std::uint32_t stall_{};
    std::uint32_t base_{};
    std::uint32_t offset_{};
    std::uint32_t error_word_{};
    std::vector<CallFrame> call_stack_;
    std::vector<Command> trace_;
    std::uint64_t executed_{};
    RunStatus status_{RunStatus::Completed};
    bool done_{};
    bool bounding_box_visible_{true};
};

std::string format_command(const Command &command);
std::string format_trace(const std::vector<Command> &trace);
std::string format_trace(const DisplayList &list);

} // namespace gungnir::ge

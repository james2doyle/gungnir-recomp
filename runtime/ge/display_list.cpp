#include "ge/display_list.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace gungnir::ge {
namespace {

std::array<std::string, 256> make_opcode_names() {
    std::array<std::string, 256> names{};
    const auto set = [&names](std::uint8_t opcode, const char *name) { names[opcode] = name; };
    const auto range = [&names](std::uint8_t first, std::uint8_t last, const char *prefix) {
        for (std::uint16_t i = first; i <= last; ++i)
            names[i] = std::string(prefix) + std::to_string(i - first);
    };

    set(0x00, "NOP");
    set(0x01, "VADDR");
    set(0x02, "IADDR");
    set(0x04, "PRIM");
    set(0x05, "BEZIER");
    set(0x06, "SPLINE");
    set(0x07, "BOUNDINGBOX");
    set(0x08, "JUMP");
    set(0x09, "BJUMP");
    set(0x0A, "CALL");
    set(0x0B, "RET");
    set(0x0C, "END");
    set(0x0E, "SIGNAL");
    set(0x0F, "FINISH");
    set(0x10, "BASE");
    set(0x12, "VTYPE");
    set(0x13, "OFFSET");
    set(0x14, "ORIGIN");
    set(0x15, "REGION1");
    set(0x16, "REGION2");
    set(0x17, "LIGHTINGENABLE");
    range(0x18, 0x1B, "LIGHTENABLE");
    set(0x1C, "DEPTHCLAMPENABLE");
    set(0x1D, "CULLFACEENABLE");
    set(0x1E, "TEXTUREMAPENABLE");
    set(0x1F, "FOGENABLE");
    set(0x20, "DITHERENABLE");
    set(0x21, "ALPHABLENDENABLE");
    set(0x22, "ALPHATESTENABLE");
    set(0x23, "ZTESTENABLE");
    set(0x24, "STENCILTESTENABLE");
    set(0x25, "ANTIALIASENABLE");
    set(0x26, "PATCHCULLENABLE");
    set(0x27, "COLORTESTENABLE");
    set(0x28, "LOGICOPENABLE");
    set(0x2A, "BONEMATRIXNUMBER");
    set(0x2B, "BONEMATRIXDATA");
    range(0x2C, 0x33, "MORPHWEIGHT");
    set(0x36, "PATCHDIVISION");
    set(0x37, "PATCHPRIMITIVE");
    set(0x38, "PATCHFACING");
    set(0x3A, "WORLDMATRIXNUMBER");
    set(0x3B, "WORLDMATRIXDATA");
    set(0x3C, "VIEWMATRIXNUMBER");
    set(0x3D, "VIEWMATRIXDATA");
    set(0x3E, "PROJMATRIXNUMBER");
    set(0x3F, "PROJMATRIXDATA");
    set(0x40, "TGENMATRIXNUMBER");
    set(0x41, "TGENMATRIXDATA");
    set(0x42, "VIEWPORTXSCALE");
    set(0x43, "VIEWPORTYSCALE");
    set(0x44, "VIEWPORTZSCALE");
    set(0x45, "VIEWPORTXCENTER");
    set(0x46, "VIEWPORTYCENTER");
    set(0x47, "VIEWPORTZCENTER");
    set(0x48, "TEXSCALEU");
    set(0x49, "TEXSCALEV");
    set(0x4A, "TEXOFFSETU");
    set(0x4B, "TEXOFFSETV");
    set(0x4C, "OFFSETX");
    set(0x4D, "OFFSETY");
    set(0x50, "SHADEMODE");
    set(0x51, "REVERSENORMAL");
    set(0x53, "MATERIALUPDATE");
    set(0x54, "MATERIALEMISSIVE");
    set(0x55, "MATERIALAMBIENT");
    set(0x56, "MATERIALDIFFUSE");
    set(0x57, "MATERIALSPECULAR");
    set(0x58, "MATERIALALPHA");
    set(0x5B, "MATERIALSPECULARCOEF");
    set(0x5C, "AMBIENTCOLOR");
    set(0x5D, "AMBIENTALPHA");
    set(0x5E, "LIGHTMODE");
    range(0x5F, 0x62, "LIGHTTYPE");
    range(0x63, 0x6E, "LIGHTPOSITION");
    range(0x6F, 0x7A, "LIGHTDIRECTION");
    range(0x7B, 0x86, "LIGHTATTENUATION");
    range(0x87, 0x8A, "LIGHTSPOTEXPONENT");
    range(0x8B, 0x8E, "LIGHTSPOTCUTOFF");
    range(0x8F, 0x9A, "LIGHTCOLOR");
    set(0x9B, "CULL");
    set(0x9C, "FRAMEBUFPTR");
    set(0x9D, "FRAMEBUFWIDTH");
    set(0x9E, "ZBUFPTR");
    set(0x9F, "ZBUFWIDTH");
    range(0xA0, 0xA7, "TEXADDR");
    range(0xA8, 0xAF, "TEXBUFWIDTH");
    set(0xB0, "CLUTADDR");
    set(0xB1, "CLUTADDRUPPER");
    set(0xB2, "TRANSFERSRC");
    set(0xB3, "TRANSFERSRCW");
    set(0xB4, "TRANSFERDST");
    set(0xB5, "TRANSFERDSTW");
    range(0xB8, 0xBF, "TEXSIZE");
    set(0xC0, "TEXMAPMODE");
    set(0xC1, "TEXSHADELS");
    set(0xC2, "TEXMODE");
    set(0xC3, "TEXFORMAT");
    set(0xC4, "LOADCLUT");
    set(0xC5, "CLUTFORMAT");
    set(0xC6, "TEXFILTER");
    set(0xC7, "TEXWRAP");
    set(0xC8, "TEXLEVEL");
    set(0xC9, "TEXFUNC");
    set(0xCA, "TEXENVCOLOR");
    set(0xCB, "TEXFLUSH");
    set(0xCC, "TEXSYNC");
    set(0xCD, "FOG1");
    set(0xCE, "FOG2");
    set(0xCF, "FOGCOLOR");
    set(0xD0, "TEXLODSLOPE");
    set(0xD2, "FRAMEBUFPIXFORMAT");
    set(0xD3, "CLEARMODE");
    set(0xD4, "SCISSOR1");
    set(0xD5, "SCISSOR2");
    set(0xD6, "MINZ");
    set(0xD7, "MAXZ");
    set(0xD8, "COLORTEST");
    set(0xD9, "COLORREF");
    set(0xDA, "COLORTESTMASK");
    set(0xDB, "ALPHATEST");
    set(0xDC, "STENCILTEST");
    set(0xDD, "STENCILOP");
    set(0xDE, "ZTEST");
    set(0xDF, "BLENDMODE");
    set(0xE0, "BLENDFIXEDA");
    set(0xE1, "BLENDFIXEDB");
    range(0xE2, 0xE5, "DITHER");
    set(0xE6, "LOGICOP");
    set(0xE7, "ZWRITEDISABLE");
    set(0xE8, "MASKRGB");
    set(0xE9, "MASKALPHA");
    set(0xEA, "TRANSFERSTART");
    set(0xEB, "TRANSFERSRCPOS");
    set(0xEC, "TRANSFERDSTPOS");
    set(0xEE, "TRANSFERSIZE");
    set(0xF0, "VSCX");
    set(0xF1, "VSCY");
    set(0xF2, "VSCZ");
    set(0xF3, "VTCS");
    set(0xF4, "VTCT");
    set(0xF5, "VTCQ");
    set(0xF6, "VCV");
    set(0xF7, "VAP");
    set(0xF8, "VFC");
    set(0xF9, "VSCV");
    set(0xFF, "NOP_FF");
    return names;
}

const std::array<std::string, 256> &opcode_names() {
    static const std::array<std::string, 256> names = make_opcode_names();
    return names;
}

CommandKind kind_for_opcode(std::uint8_t opcode) {
    switch (opcode) {
    case 0x00:
    case 0xFF:
        return CommandKind::Nop;
    case 0x01:
        return CommandKind::VertexAddress;
    case 0x02:
        return CommandKind::IndexAddress;
    case 0x04:
        return CommandKind::Primitive;
    case 0x05:
        return CommandKind::Bezier;
    case 0x06:
        return CommandKind::Spline;
    case 0x07:
        return CommandKind::BoundingBox;
    case 0x08:
        return CommandKind::Jump;
    case 0x09:
        return CommandKind::BranchJump;
    case 0x0A:
        return CommandKind::Call;
    case 0x0B:
        return CommandKind::Return;
    case 0x0C:
        return CommandKind::End;
    case 0x0E:
        return CommandKind::Signal;
    case 0x0F:
        return CommandKind::Finish;
    case 0x10:
        return CommandKind::Base;
    case 0x12:
        return CommandKind::VertexType;
    case 0x13:
        return CommandKind::Offset;
    case 0x14:
        return CommandKind::Origin;
    default:
        return is_defined_opcode(opcode) ? CommandKind::State : CommandKind::Unknown;
    }
}

int align_up(int value, int alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

std::string hex32(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%08X", value);
    return buffer;
}

std::string hex24(std::uint32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof buffer, "%06X", value);
    return buffer;
}

} // namespace

const char *primitive_name(Primitive primitive) {
    static const char *const names[] = {
        "points", "lines", "line_strip", "triangles", "triangle_strip", "triangle_fan", "sprites", "invalid"};
    const auto index = static_cast<unsigned>(primitive);
    return index < 8u ? names[index] : "invalid";
}

VertexLayout decode_vertex_layout(std::uint32_t vertex_type) {
    VertexLayout layout;
    layout.raw = vertex_type;
    layout.texcoord = static_cast<std::uint8_t>(vertex_type & 3u);
    layout.color = static_cast<std::uint8_t>((vertex_type >> 2u) & 7u);
    layout.normal = static_cast<std::uint8_t>((vertex_type >> 5u) & 3u);
    layout.position = static_cast<std::uint8_t>((vertex_type >> 7u) & 3u);
    layout.weight = static_cast<std::uint8_t>((vertex_type >> 9u) & 3u);
    layout.index = static_cast<std::uint8_t>((vertex_type >> 11u) & 3u);
    layout.weight_count = static_cast<std::uint8_t>(((vertex_type >> 14u) & 7u) + 1u);
    layout.morph_count = static_cast<std::uint8_t>(((vertex_type >> 18u) & 7u) + 1u);
    layout.through = ((vertex_type >> 23u) & 1u) != 0u;
    layout.index_size = layout.index == 1u ? 1u : layout.index == 2u ? 2u : layout.index == 3u ? 4u : 0u;

    static constexpr int scalar_size[4] = {0, 1, 2, 4};
    int offset = 0;
    int max_alignment = 1;
    const auto place = [&offset, &max_alignment](int size, int alignment) {
        offset = align_up(offset, alignment);
        const int at = offset;
        offset += size;
        max_alignment = std::max(max_alignment, alignment);
        return at;
    };

    if (layout.weight != 0u) {
        const int size = scalar_size[layout.weight];
        layout.weight_offset = static_cast<std::uint16_t>(place(size * layout.weight_count, size));
    }
    if (layout.texcoord != 0u) {
        const int size = scalar_size[layout.texcoord];
        layout.texcoord_offset = static_cast<std::uint16_t>(place(size * 2, size));
    }
    if (layout.color >= 4u) {
        const int size = layout.color == 7u ? 4 : 2;
        layout.color_offset = static_cast<std::uint16_t>(place(size, size));
    }
    if (layout.normal != 0u) {
        const int size = scalar_size[layout.normal];
        layout.normal_offset = static_cast<std::uint16_t>(place(size * 3, size));
    }
    const int position_size = scalar_size[layout.position != 0u ? layout.position : 3u];
    layout.position_offset = static_cast<std::uint16_t>(place(position_size * 3, position_size));

    const int vertex_size = align_up(offset, max_alignment);
    layout.stride = static_cast<std::uint16_t>(vertex_size * layout.morph_count);
    return layout;
}

const char *kind_name(CommandKind kind) {
    switch (kind) {
    case CommandKind::Nop:
        return "nop";
    case CommandKind::VertexAddress:
        return "vertex_address";
    case CommandKind::IndexAddress:
        return "index_address";
    case CommandKind::Primitive:
        return "primitive";
    case CommandKind::Bezier:
        return "bezier";
    case CommandKind::Spline:
        return "spline";
    case CommandKind::BoundingBox:
        return "bounding_box";
    case CommandKind::Jump:
        return "jump";
    case CommandKind::BranchJump:
        return "branch_jump";
    case CommandKind::Call:
        return "call";
    case CommandKind::Return:
        return "return";
    case CommandKind::End:
        return "end";
    case CommandKind::Signal:
        return "signal";
    case CommandKind::Finish:
        return "finish";
    case CommandKind::Base:
        return "base";
    case CommandKind::VertexType:
        return "vertex_type";
    case CommandKind::Offset:
        return "offset";
    case CommandKind::Origin:
        return "origin";
    case CommandKind::State:
        return "state";
    case CommandKind::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::uint32_t decode_word_opcode(std::uint32_t word) {
    return word >> 24u;
}

std::uint32_t decode_word_argument(std::uint32_t word) {
    return word & 0xFFFFFFu;
}

bool is_defined_opcode(std::uint8_t opcode) {
    return !opcode_names()[opcode].empty();
}

const char *opcode_name(std::uint8_t opcode) {
    const std::string &name = opcode_names()[opcode];
    return name.empty() ? "UNKNOWN" : name.c_str();
}

Command decode_command(std::uint32_t pc, std::uint32_t word) {
    Command command;
    command.pc = pc;
    command.word = word;
    command.opcode = static_cast<std::uint8_t>(word >> 24u);
    command.argument = word & 0xFFFFFFu;
    command.kind = kind_for_opcode(command.opcode);

    switch (command.kind) {
    case CommandKind::Primitive:
        command.primitive = static_cast<Primitive>((command.argument >> 16u) & 7u);
        command.count = static_cast<std::uint16_t>(command.argument & 0xFFFFu);
        break;
    case CommandKind::VertexType:
        command.vertex_layout = decode_vertex_layout(command.argument);
        break;
    case CommandKind::Base:
        command.base = (command.argument & 0x00FF0000u) << 8u;
        break;
    case CommandKind::Offset:
        command.offset = command.argument << 8u;
        break;
    default:
        break;
    }
    return command;
}

const char *run_status_name(RunStatus status) {
    switch (status) {
    case RunStatus::Completed:
        return "completed";
    case RunStatus::Stalled:
        return "stalled";
    case RunStatus::UnknownOpcode:
        return "unknown_opcode";
    case RunStatus::InvalidPrimitive:
        return "invalid_primitive";
    case RunStatus::CommandLimit:
        return "command_limit";
    case RunStatus::CallDepthExceeded:
        return "call_depth_exceeded";
    case RunStatus::ReturnWithoutCall:
        return "return_without_call";
    case RunStatus::MemoryFault:
        return "memory_fault";
    }
    return "unknown";
}

DisplayList::DisplayList(WordReader reader) : reader_(std::move(reader)) {}

DisplayList::DisplayList(WordReader reader, Config config) : reader_(std::move(reader)), config_(std::move(config)) {}

void DisplayList::reset(std::uint32_t start, std::uint32_t stall) {
    pc_ = start & 0x0FFFFFFFu;
    stall_ = stall & 0x0FFFFFFFu;
    base_ = 0u;
    offset_ = 0u;
    error_word_ = 0u;
    call_stack_.clear();
    trace_.clear();
    executed_ = 0u;
    status_ = RunStatus::Completed;
    done_ = false;
    bounding_box_visible_ = true;
}

void DisplayList::set_stall(std::uint32_t stall) {
    stall_ = stall & 0x0FFFFFFFu;
}

std::uint32_t DisplayList::resolve(std::uint32_t argument) const {
    return ((base_ | argument) + offset_) & 0x0FFFFFFFu;
}

RunResult DisplayList::finish(RunStatus status, std::uint32_t word) {
    status_ = status;
    if (status == RunStatus::Completed) done_ = true;
    if (status != RunStatus::Completed && status != RunStatus::Stalled) error_word_ = word;
    return {status, pc_, word};
}

void DisplayList::record(const Command &command) {
    if (config_.record_trace) trace_.push_back(command);
    if (config_.on_command) config_.on_command(command);
}

RunResult DisplayList::run() {
    if (done_) return {RunStatus::Completed, pc_, 0u};
    if (status_ != RunStatus::Completed && status_ != RunStatus::Stalled)
        return {status_, pc_, error_word_};
    status_ = RunStatus::Completed;

    for (;;) {
        if (stall_ != 0u && pc_ == stall_) return finish(RunStatus::Stalled, 0u);
        if (executed_ >= config_.max_commands) return finish(RunStatus::CommandLimit, 0u);
        if (!reader_) return finish(RunStatus::MemoryFault, 0u);
        const std::optional<std::uint32_t> fetched = reader_(pc_);
        if (!fetched) return finish(RunStatus::MemoryFault, 0u);

        const std::uint32_t word = *fetched;
        const std::uint32_t command_pc = pc_;
        Command command = decode_command(command_pc, word);
        ++executed_;
        pc_ = command_pc + 4u;

        RunStatus error = RunStatus::Completed;
        bool stop = false;
        switch (command.kind) {
        case CommandKind::Nop:
        case CommandKind::Bezier:
        case CommandKind::Spline:
        case CommandKind::Signal:
        case CommandKind::Finish:
        case CommandKind::State:
            break;
        case CommandKind::VertexAddress:
        case CommandKind::IndexAddress:
            command.address = resolve(command.argument);
            break;
        case CommandKind::Primitive:
            if (command.primitive == Primitive::Invalid) error = RunStatus::InvalidPrimitive;
            break;
        case CommandKind::BoundingBox:
            bounding_box_visible_ = true;
            break;
        case CommandKind::Jump:
            command.address = resolve(command.argument & ~3u);
            pc_ = command.address;
            break;
        case CommandKind::BranchJump:
            command.address = resolve(command.argument & ~3u);
            command.branch_taken = !bounding_box_visible_;
            if (command.branch_taken) pc_ = command.address;
            break;
        case CommandKind::Call:
            if (call_stack_.size() >= 2u) {
                error = RunStatus::CallDepthExceeded;
                break;
            }
            command.address = resolve(command.argument & ~3u);
            call_stack_.push_back({pc_, offset_});
            pc_ = command.address;
            break;
        case CommandKind::Return: {
            if (call_stack_.empty()) {
                error = RunStatus::ReturnWithoutCall;
                break;
            }
            const CallFrame frame = call_stack_.back();
            call_stack_.pop_back();
            offset_ = frame.offset;
            command.address = frame.return_pc;
            pc_ = frame.return_pc;
            break;
        }
        case CommandKind::End:
            stop = true;
            break;
        case CommandKind::Base:
            base_ = command.base;
            break;
        case CommandKind::VertexType:
            break;
        case CommandKind::Offset:
            offset_ = command.offset;
            break;
        case CommandKind::Origin:
            offset_ = command_pc;
            command.offset = offset_;
            break;
        case CommandKind::Unknown:
            error = RunStatus::UnknownOpcode;
            break;
        }

        record(command);
        if (error != RunStatus::Completed) {
            pc_ = command_pc;
            return finish(error, word);
        }
        if (stop) return finish(RunStatus::Completed, word);
    }
}

std::string format_command(const Command &command) {
    std::string line = hex32(command.pc) + "  ";
    char opcode[8];
    std::snprintf(opcode, sizeof opcode, "%02X", command.opcode);
    line += opcode;
    line += " ";
    line += hex24(command.argument);
    line += "  ";

    switch (command.kind) {
    case CommandKind::Nop:
        line += command.opcode == 0xFFu ? "NOP_FF" : "NOP";
        break;
    case CommandKind::VertexAddress:
        line += "VADDR " + hex24(command.argument) + " -> " + hex32(command.address);
        break;
    case CommandKind::IndexAddress:
        line += "IADDR " + hex24(command.argument) + " -> " + hex32(command.address);
        break;
    case CommandKind::Primitive:
        line += std::string("PRIM ") + primitive_name(command.primitive) + " x" + std::to_string(command.count);
        break;
    case CommandKind::Bezier:
        line += "BEZIER " + hex24(command.argument);
        break;
    case CommandKind::Spline:
        line += "SPLINE " + hex24(command.argument);
        break;
    case CommandKind::BoundingBox:
        line += "BOUNDINGBOX " + hex24(command.argument);
        break;
    case CommandKind::Jump:
        line += "JUMP " + hex24(command.argument) + " -> " + hex32(command.address);
        break;
    case CommandKind::BranchJump:
        line += "BJUMP " + hex24(command.argument) + " -> " + hex32(command.address);
        line += command.branch_taken ? " taken" : " fallthrough";
        break;
    case CommandKind::Call:
        line += "CALL " + hex24(command.argument) + " -> " + hex32(command.address);
        break;
    case CommandKind::Return:
        line += command.address != 0u ? "RET -> " + hex32(command.address) : "RET";
        break;
    case CommandKind::End:
        line += "END " + hex24(command.argument);
        break;
    case CommandKind::Signal:
        line += "SIGNAL " + hex24(command.argument);
        break;
    case CommandKind::Finish:
        line += "FINISH " + hex24(command.argument);
        break;
    case CommandKind::Base:
        line += "BASE " + hex32(command.base);
        break;
    case CommandKind::VertexType: {
        const VertexLayout &layout = command.vertex_layout;
        line += "VTYPE tc=" + std::to_string(layout.texcoord) + " color=" + std::to_string(layout.color) +
                " normal=" + std::to_string(layout.normal) + " pos=" + std::to_string(layout.position) +
                " weight=" + std::to_string(layout.weight) + " index=" + std::to_string(layout.index) +
                " weights=" + std::to_string(layout.weight_count) + " morphs=" + std::to_string(layout.morph_count) +
                " stride=" + std::to_string(layout.stride) + " through=" + (layout.through ? "1" : "0");
        break;
    }
    case CommandKind::Offset:
        line += "OFFSET " + hex32(command.offset);
        break;
    case CommandKind::Origin:
        line += "ORIGIN " + hex32(command.offset);
        break;
    case CommandKind::State:
        line += opcode_name(command.opcode);
        line += " " + hex24(command.argument);
        break;
    case CommandKind::Unknown:
        line += "UNKNOWN";
        break;
    }
    return line;
}

std::string format_trace(const std::vector<Command> &trace) {
    std::string text;
    for (const Command &command : trace) {
        text += format_command(command);
        text += '\n';
    }
    return text;
}

std::string format_trace(const DisplayList &list) {
    return format_trace(list.trace());
}

} // namespace gungnir::ge

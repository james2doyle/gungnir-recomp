// T13b — GE state machine contracts.
//
// Inputs are synthetic command words and vertices of our authorship.

#include "ge/state_machine.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace gungnir::ge;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool close(float a, float b, float tolerance = 1.0e-5f) {
    return std::fabs(a - b) <= tolerance;
}

class Memory {
public:
    explicit Memory(std::size_t size = 0x10000u) : bytes_(size, 0u) {}

    void store32(std::uint32_t address, std::uint32_t value) {
        std::memcpy(bytes_.data() + address, &value, sizeof value);
    }

    void store_float(std::uint32_t address, float value) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        store32(address, bits);
    }

    void store8(std::uint32_t address, std::uint8_t value) { bytes_[address] = value; }

    bool read(std::uint32_t address, std::uint8_t *out, std::size_t size) const {
        if (address + size > bytes_.size()) return false;
        std::memcpy(out, bytes_.data() + address, size);
        return true;
    }

private:
    std::vector<std::uint8_t> bytes_;
};

MemoryView view(const Memory &memory) {
    return [&memory](std::uint32_t address, std::uint8_t *out, std::size_t size) {
        return memory.read(address, out, size);
    };
}

Command command(CommandKind kind, std::uint32_t argument = 0u, std::uint8_t opcode = 0u) {
    Command cmd;
    cmd.kind = kind;
    cmd.opcode = opcode;
    cmd.argument = argument;
    if (kind == CommandKind::VertexAddress || kind == CommandKind::IndexAddress)
        cmd.address = argument;
    if (kind == CommandKind::Primitive) {
        cmd.primitive = static_cast<Primitive>((argument >> 16u) & 7u);
        cmd.count = static_cast<std::uint16_t>(argument & 0xFFFFu);
    }
    if (kind == CommandKind::VertexType) cmd.vertex_layout = decode_vertex_layout(argument);
    return cmd;
}

void write_through_triangle(Memory &memory, std::uint32_t address) {
    const VertexLayout layout = decode_vertex_layout((3u << 7u) | 3u | (7u << 2u) | (1u << 23u));
    const float positions[3][2] = {{10.0f, 20.0f}, {50.0f, 20.0f}, {10.0f, 60.0f}};
    const std::array<std::uint32_t, 3> colors{0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t at = address + static_cast<std::uint32_t>(i) * layout.stride;
        memory.store_float(at + layout.position_offset + 0u, positions[i][0]);
        memory.store_float(at + layout.position_offset + 4u, positions[i][1]);
        memory.store_float(at + layout.position_offset + 8u, 0.0f);
        memory.store_float(at + layout.texcoord_offset + 0u, 0.0f);
        memory.store_float(at + layout.texcoord_offset + 4u, 0.0f);
        memory.store32(at + layout.color_offset, colors[static_cast<std::size_t>(i)]);
    }
}

void through_triangle() {
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });

    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));

    check(draws.size() == 1u, "one through-mode draw");
    if (draws.size() != 1u) return;
    const Draw &draw = draws[0];
    check(draw.vertices.size() == 3u, "three screen vertices");
    check(draw.indices.size() == 3u, "three indices");
    check(close(draw.vertices[0].x, 10.0f) && close(draw.vertices[0].y, 20.0f),
        "through position is screen space");
    check(close(draw.vertices[1].x, 50.0f), "second screen x");
    check(draw.vertices[0].color[0] == 1.0f && draw.vertices[0].color[3] == 1.0f,
        "8888 color decoded");
}

void indexed_and_textured() {
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    const std::uint32_t vt = (3u << 7u) | 3u | (7u << 2u) | (1u << 23u) | (2u << 11u);
    memory.store8(0x2000u, 0u);
    memory.store8(0x2001u, 1u);
    memory.store8(0x2002u, 2u);
    for (int i = 0; i < 4; ++i) memory.store32(0x3000u + static_cast<std::uint32_t>(i) * 4u,
        0xFF000000u | static_cast<std::uint32_t>(i * 40u) << 16u);

    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    const auto send = [&](CommandKind kind, std::uint32_t argument, std::uint8_t opcode) {
        machine.command(command(kind, argument, opcode), view(memory));
    };
    send(CommandKind::VertexType, vt, 0x12u);
    send(CommandKind::VertexAddress, 0x1000u, 0x01u);
    send(CommandKind::IndexAddress, 0x2000u, 0x02u);
    send(CommandKind::State, 1u, 0x1Eu);       // TEXTUREMAPENABLE
    send(CommandKind::State, 0x3000u, 0xA0u);  // TEXADDR0
    send(CommandKind::State, 2u, 0xA8u);       // TEXBUFWIDTH0
    send(CommandKind::State, 0x0101u, 0xB8u);  // TEXSIZE0: 2x2
    send(CommandKind::State, 3u, 0xC3u);       // TEXFORMAT: RGBA8888
    send(CommandKind::State, 0u, 0xC2u);       // TEXMODE
    send(CommandKind::Primitive, (6u << 16u) | 2u, 0x04u);  // sprites x2? use triangles instead

    check(draws.size() == 1u, "one indexed draw");
    if (draws.size() != 1u) return;
    check(draws[0].state.texture != nullptr, "texture decoded");
    check(draws[0].state.texture != nullptr && draws[0].state.texture->width == 2 &&
              draws[0].state.texture->height == 2,
        "texture extent");
}

void through_sprite_quad() {
    Memory memory;
    const std::uint32_t layout_bits = (3u << 7u) | 3u | (7u << 2u) | (1u << 23u);
    const VertexLayout layout = decode_vertex_layout(layout_bits);
    const std::array<std::array<float, 2>, 2> positions{{{10.0f, 20.0f}, {50.0f, 60.0f}}};
    const std::array<std::array<float, 2>, 2> texcoords{{{64.0f, 0.0f}, {128.0f, 32.0f}}};
    for (int i = 0; i < 2; ++i) {
        const std::uint32_t at = 0x1000u + static_cast<std::uint32_t>(i) * layout.stride;
        memory.store_float(at + layout.position_offset + 0u, positions[static_cast<std::size_t>(i)][0]);
        memory.store_float(at + layout.position_offset + 4u, positions[static_cast<std::size_t>(i)][1]);
        memory.store_float(at + layout.position_offset + 8u, 0.0f);
        memory.store_float(at + layout.texcoord_offset + 0u, texcoords[static_cast<std::size_t>(i)][0]);
        memory.store_float(at + layout.texcoord_offset + 4u, texcoords[static_cast<std::size_t>(i)][1]);
        memory.store32(at + layout.color_offset, 0xFFFFFFFFu);
    }

    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    machine.command(command(CommandKind::VertexType, layout_bits, 0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (6u << 16u) | 2u, 0x04u), view(memory));

    check(draws.size() == 1u, "one through-mode sprite draw");
    if (draws.size() != 1u) return;
    const Draw &draw = draws[0];
    check(draw.vertices.size() == 4u, "sprite expands to four corners");
    if (draw.vertices.size() != 4u) return;
    check(close(draw.vertices[0].x, 10.0f) && close(draw.vertices[0].y, 20.0f), "sprite top-left position");
    check(close(draw.vertices[1].x, 50.0f) && close(draw.vertices[1].y, 20.0f), "sprite top-right position");
    check(close(draw.vertices[2].x, 10.0f) && close(draw.vertices[2].y, 60.0f), "sprite bottom-left position");
    check(close(draw.vertices[3].x, 50.0f) && close(draw.vertices[3].y, 60.0f), "sprite bottom-right position");
    check(close(draw.vertices[2].texcoord[0], 64.0f) && close(draw.vertices[2].texcoord[1], 32.0f),
          "sprite bottom-left texcoord");
}

void scissor_and_gaps() {
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    std::vector<std::string> gaps;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    machine.set_gap_sink([&gaps](const std::string &gap) { gaps.push_back(gap); });
    machine.command(command(CommandKind::State, (100u << 10u) | 20u, 0xD4u), view(memory));
    machine.command(command(CommandKind::State, (200u << 10u) | 220u, 0xD5u), view(memory));
    machine.command(command(CommandKind::State, 1u, 0x21u), view(memory));
    machine.command(command(CommandKind::State, 2u | (3u << 4u), 0xDFu), view(memory));
    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));
    machine.command(command(CommandKind::Primitive, (1u << 16u) | 4u, 0x04u), view(memory));

    check(draws.size() == 2u, "lines primitive emits a draw");
    check(draws[0].state.scissor_x0 == 20 && draws[0].state.scissor_y0 == 100 &&
              draws[0].state.scissor_x1 == 221 && draws[0].state.scissor_y1 == 201,
        "scissor decoded");
    check(draws[0].state.blend.enabled && draws[0].state.blend.source == BlendFactor::SrcAlpha &&
              draws[0].state.blend.destination == BlendFactor::OneMinusSrcAlpha,
        "blend decoded");
    for (const std::string &gap : gaps)
        check(gap.find("primitive_unsupported") == std::string::npos, "lines are no longer a gap");
}

void blend_factor_table() {
    // pspsdk pspgu.h: 0 = other color (dst for src operand, src for dst
    // operand), 1 = one-minus-other, 2/3 src alpha, 4/5 dst alpha,
    // 6/7 doubled src alpha, 8/9 doubled dst alpha, 10+ = FIX register.
    // The event-scene world draws use src=0/dst=1; decoding those as
    // (Zero, One) turns the lighting pass into a no-op and the world black.
    struct Case {
        std::uint32_t mode;
        BlendFactor source;
        BlendFactor destination;
    };
    const Case cases[] = {
        {0u | (1u << 4u), BlendFactor::DstColor, BlendFactor::OneMinusSrcColor},
        {2u | (3u << 4u), BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha},
        {4u | (5u << 4u), BlendFactor::DstAlpha, BlendFactor::OneMinusDstAlpha},
        {6u | (7u << 4u), BlendFactor::DoubleSrcAlpha, BlendFactor::DoubleOneMinusSrcAlpha},
        {8u | (9u << 4u), BlendFactor::DoubleDstAlpha, BlendFactor::DoubleOneMinusDstAlpha},
        {10u | (10u << 4u), BlendFactor::Fix, BlendFactor::Fix},
        {15u | (11u << 4u), BlendFactor::Fix, BlendFactor::Fix},
    };
    for (const Case &entry : cases) {
        Memory memory;
        write_through_triangle(memory, 0x1000u);
        StateMachine machine;
        std::vector<Draw> draws;
        machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
        machine.command(command(CommandKind::State, 0x00223344u, 0xE0u), view(memory));
        machine.command(command(CommandKind::State, 0x00667788u, 0xE1u), view(memory));
        machine.command(command(CommandKind::State, 1u, 0x21u), view(memory));
        machine.command(command(CommandKind::State, entry.mode, 0xDFu), view(memory));
        machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                               0x12u), view(memory));
        machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
        machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));
        const std::string what = "blend mode 0x" + std::to_string(entry.mode);
        check(!draws.empty(), what + " emits a draw");
        if (draws.empty()) continue;
        check(draws[0].state.blend.source == entry.source, what + " source factor");
        check(draws[0].state.blend.destination == entry.destination, what + " destination factor");
        if (entry.source == BlendFactor::Fix)
            check(draws[0].state.blend.fixed_source == 0x00223344u, what + " fixed source color");
        if (entry.destination == BlendFactor::Fix)
            check(draws[0].state.blend.fixed_destination == 0x00667788u,
                  what + " fixed destination color");
    }
}

void clear_and_write_mask() {
    // CLEAR mode (0xD3 bit 0) turns a draw into a clear; the write mask
    // comes from the clear bits, otherwise from PMASK (0xE8/0xE9, bits
    // set = keep destination). Blending is forced off for clears.
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    machine.command(command(CommandKind::State, 1u, 0xD3u), view(memory));
    machine.command(command(CommandKind::State, 1u, 0x21u), view(memory));
    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));
    check(!draws.empty(), "clear draw emits");
    if (draws.empty()) return;
    check(draws[0].state.clear, "CLEAR mode flagged");
    check(!draws[0].state.blend.enabled, "clear forces blending off");
    check(!draws[0].state.texture_enabled, "clear disables texturing");

    Memory masked;
    write_through_triangle(masked, 0x1000u);
    StateMachine keeper;
    std::vector<Draw> kept;
    keeper.set_draw_sink([&kept](const Draw &draw) { kept.push_back(draw); });
    keeper.command(command(CommandKind::State, 0x00FFFF00u, 0xE8u), view(masked));
    keeper.command(command(CommandKind::State, 0x000000FFu, 0xE9u), view(masked));
    keeper.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                          0x12u), view(masked));
    keeper.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(masked));
    keeper.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(masked));
    check(!kept.empty(), "masked draw emits");
    if (kept.empty()) return;
    check(!kept[0].state.clear, "normal draw is not a clear");
    check(kept[0].state.write_mask == 0xFFFFFF00u, "PMASK latched as the write mask");
}

void alpha_test_decode() {
    // 0x22 enables the test, 0xDB carries func/ref/mask. Previously this
    // only logged alpha_test_unsupported and rendered unconditionally.
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    machine.command(command(CommandKind::State, 1u, 0x22u), view(memory));
    machine.command(command(CommandKind::State, (6u | (128u << 8u) | (0xFFu << 16u)), 0xDBu),
                    view(memory));
    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));
    check(!draws.empty(), "alpha-tested draw emits");
    if (draws.empty()) return;
    check(draws[0].state.alpha_test, "alpha test enabled decoded");
    check(draws[0].state.alpha_function == CompareFunc::Greater, "alpha func decoded");
    check(draws[0].state.alpha_reference == 128u, "alpha reference decoded");
}

void fog_decode() {
    // 0x1F enables fog, 0xCD end / 0xCE slope / 0xCF color. Transform-mode
    // only: through draws never fog even with the enable set.
    auto encode_f24 = [](float value) {
        std::uint32_t bits = 0u;
        std::memcpy(&bits, &value, sizeof bits);
        return bits >> 8u;
    };
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    machine.command(command(CommandKind::State, 1u, 0x1Fu), view(memory));
    machine.command(command(CommandKind::State, encode_f24(1.0f), 0xCDu), view(memory));
    machine.command(command(CommandKind::State, encode_f24(1.0f), 0xCEu), view(memory));
    machine.command(command(CommandKind::State, 0x0000FF00u, 0xCFu), view(memory));
    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(memory));
    check(!draws.empty(), "fogged draw emits");
    if (draws.empty()) return;
    check(!draws[0].state.fog_enabled, "through-mode draw never fogs");

    // Transform-mode with the enable set latches the flag and the color.
    Memory world;
    write_through_triangle(world, 0x1000u);
    StateMachine transform_machine;
    std::vector<Draw> world_draws;
    transform_machine.set_draw_sink([&world_draws](const Draw &draw) { world_draws.push_back(draw); });
    transform_machine.command(command(CommandKind::State, 1u, 0x1Fu), view(world));
    transform_machine.command(command(CommandKind::State, encode_f24(1.0f), 0xCDu), view(world));
    transform_machine.command(command(CommandKind::State, encode_f24(1.0f), 0xCEu), view(world));
    transform_machine.command(command(CommandKind::State, 0x0000FF00u, 0xCFu), view(world));
    transform_machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u), 0x12u),
                              view(world));
    transform_machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(world));
    transform_machine.command(command(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u), view(world));
    check(!world_draws.empty(), "transform-mode fogged draw emits");
    if (world_draws.empty()) return;
    check(world_draws[0].state.fog_enabled, "transform-mode fog enabled decoded");
    check(world_draws[0].state.fog_color == 0x0000FF00u, "fog color latched");
}

void line_strip_emits() {
    // Points/lines used to be a named gap (primitive_unsupported) and were
    // dropped; they now emit draws the rasterizers can consume.
    Memory memory;
    write_through_triangle(memory, 0x1000u);
    StateMachine machine;
    std::vector<Draw> draws;
    machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
    std::vector<std::string> gaps;
    machine.set_gap_sink([&gaps](const std::string &message) { gaps.push_back(message); });
    machine.command(command(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u),
                           0x12u), view(memory));
    machine.command(command(CommandKind::VertexAddress, 0x1000u, 0x01u), view(memory));
    machine.command(command(CommandKind::Primitive, (2u << 16u) | 3u, 0x04u), view(memory));
    check(draws.size() == 1u, "one line-strip draw");
    if (draws.empty()) return;
    check(draws[0].primitive == Primitive::LineStrip, "line-strip primitive preserved");
    check(draws[0].indices.size() == 4u, "strip of 3 expands to two segments");
    if (draws[0].indices.size() == 4u)
        check(draws[0].indices[0] == 0u && draws[0].indices[1] == 1u && draws[0].indices[2] == 2u &&
                  draws[0].indices[3] == 3u,
              "segments walk the strip in order");
    for (const std::string &gap : gaps)
        check(gap.find("primitive_unsupported") == std::string::npos, "no unsupported-primitive gap");
}

void texwrap_decode() {
    // TEXWRAP (0xC7) bit 0 clamps U, bit 8 clamps V; clear repeats. The
    // flags ride DrawState as clamp_u/v (true = clamp, matching the
    // reference tex_coord). A prior bug fed them into repeat-semantics
    // Vulkan sampler params, inverting wrap on the GPU path.
    const auto textured_draw = [](std::uint32_t wrap) {
        Memory memory;
        write_through_triangle(memory, 0x1000u);
        for (int i = 0; i < 4; ++i)
            memory.store32(0x3000u + static_cast<std::uint32_t>(i) * 4u, 0xFF000000u);
        StateMachine machine;
        std::vector<Draw> draws;
        machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
        const auto send = [&](CommandKind kind, std::uint32_t argument, std::uint8_t opcode) {
            machine.command(command(kind, argument, opcode), view(memory));
        };
        send(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u), 0x12u);
        send(CommandKind::VertexAddress, 0x1000u, 0x01u);
        send(CommandKind::State, 1u, 0x1Eu);
        send(CommandKind::State, 0x3000u, 0xA0u);
        send(CommandKind::State, 2u, 0xA8u);
        send(CommandKind::State, 0x0101u, 0xB8u);
        send(CommandKind::State, 3u, 0xC3u);
        send(CommandKind::State, 0u, 0xC2u);
        send(CommandKind::State, wrap, 0xC7u);
        send(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u);
        return draws;
    };
    auto repeat = textured_draw(0u);
    check(!repeat.empty() && repeat[0].state.texture != nullptr, "repeat-mode textured draw emits");
    if (!repeat.empty()) check(!repeat[0].state.clamp_u && !repeat[0].state.clamp_v, "wrap 0x00 repeats both axes");
    auto clamp = textured_draw(0x0101u);
    check(!clamp.empty() && clamp[0].state.texture != nullptr, "clamp-mode textured draw emits");
    if (!clamp.empty()) check(clamp[0].state.clamp_u && clamp[0].state.clamp_v, "wrap 0x0101 clamps both axes");
    auto split = textured_draw(0x0100u);
    if (!split.empty()) check(!split[0].state.clamp_u && split[0].state.clamp_v, "wrap 0x0100 clamps V only");
}

void texfunc_decode() {
    // TEXFUNC (0xC9): low 3 bits select the combine (4-7 all add),
    // bit 8 selects RGBA mode, bit 16 doubles color. TEXENVCOLOR (0xCA)
    // carries the blend color. Non-modulate funcs used to be an
    // unsupported gap and rendered as modulate.
    const auto textured_draw = [](std::uint32_t func, std::uint32_t env) {
        Memory memory;
        write_through_triangle(memory, 0x1000u);
        for (int i = 0; i < 4; ++i)
            memory.store32(0x3000u + static_cast<std::uint32_t>(i) * 4u, 0xFF000000u);
        StateMachine machine;
        std::vector<Draw> draws;
        machine.set_draw_sink([&draws](const Draw &draw) { draws.push_back(draw); });
        const auto send = [&](CommandKind kind, std::uint32_t argument, std::uint8_t opcode) {
            machine.command(command(kind, argument, opcode), view(memory));
        };
        send(CommandKind::VertexType, (3u << 7u) | 3u | (7u << 2u) | (1u << 23u), 0x12u);
        send(CommandKind::VertexAddress, 0x1000u, 0x01u);
        send(CommandKind::State, 1u, 0x1Eu);
        send(CommandKind::State, 0x3000u, 0xA0u);
        send(CommandKind::State, 2u, 0xA8u);
        send(CommandKind::State, 0x0101u, 0xB8u);
        send(CommandKind::State, 3u, 0xC3u);
        send(CommandKind::State, 0u, 0xC2u);
        send(CommandKind::State, func, 0xC9u);
        send(CommandKind::State, env, 0xCAu);
        send(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u);
        return draws;
    };
    auto decal = textured_draw(1u | 0x100u | 0x10000u, 0x00112233u);
    check(!decal.empty() && decal[0].state.texture != nullptr, "decal draw emits textured");
    if (!decal.empty()) {
        check(decal[0].state.tex_func == TextureFunc::Decal, "decal func decoded");
        check(decal[0].state.tex_rgba, "RGBA mode decoded");
        check(decal[0].state.color_double, "color double decoded");
        check(decal[0].state.tex_env == 0x00112233u, "env color latched");
    }
    auto replace = textured_draw(3u, 0u);
    check(!replace.empty() && replace[0].state.texture != nullptr, "replace draw emits textured");
    if (!replace.empty()) {
        check(replace[0].state.tex_func == TextureFunc::Replace, "replace func decoded");
        check(!replace[0].state.tex_rgba && !replace[0].state.color_double, "replace flags clear");
    }
    auto add_alias = textured_draw(6u, 0u);
    if (!add_alias.empty()) check(add_alias[0].state.tex_func == TextureFunc::Add, "func 6 aliases add");
}

void bounded_texture_retention() {
    Memory memory(0x200000u);
    write_through_triangle(memory, 0x1000u);
    constexpr std::uint32_t kTexture = 0x40000u;
    constexpr int kSize = 256;
    for (int i = 0; i < kSize * kSize; ++i)
        memory.store32(kTexture + static_cast<std::uint32_t>(i) * 4u,
                       0xFF000000u | static_cast<std::uint32_t>(i & 0xFFFF));

    StateMachine machine;
    Draw latest;
    bool have_latest = false;
    machine.set_draw_sink([&latest, &have_latest](const Draw &draw) {
        latest = draw;
        have_latest = true;
    });
    const auto send = [&](CommandKind kind, std::uint32_t argument, std::uint8_t opcode) {
        machine.command(command(kind, argument, opcode), view(memory));
    };
    const std::uint32_t vt = (3u << 7u) | 3u | (7u << 2u) | (1u << 23u);
    send(CommandKind::VertexType, vt, 0x12u);
    send(CommandKind::VertexAddress, 0x1000u, 0x01u);
    send(CommandKind::State, 1u, 0x1Eu);
    send(CommandKind::State, kTexture, 0xA0u);
    send(CommandKind::State, kSize, 0xA8u);
    send(CommandKind::State, (8u << 8u) | 8u, 0xB8u);
    send(CommandKind::State, 3u, 0xC3u);
    send(CommandKind::State, 0u, 0xC2u);

    Draw first;
    bool have_first = false;
    machine.set_draw_sink([&latest, &have_latest, &first, &have_first](const Draw &draw) {
        latest = draw;
        have_latest = true;
        if (!have_first) {
            first = draw;
            have_first = true;
        }
    });
    for (int i = 0; i < 400; ++i) {
        memory.store8(kTexture + static_cast<std::uint32_t>(i % (kSize * kSize * 4)), static_cast<std::uint8_t>(i));
        send(CommandKind::Primitive, (3u << 16u) | 3u, 0x04u);
    }

    check(have_latest && have_first, "textured draws emitted");
    check(machine.cached_texture_count() <= 300u,
          "texture cache entry count is bounded (got " + std::to_string(machine.cached_texture_count()) + ")");
    check(machine.cached_texture_bytes() <= 72u * 1024u * 1024u,
          "texture cache bytes are bounded (got " + std::to_string(machine.cached_texture_bytes()) + ")");
    check(have_first && first.state.texture != nullptr && !first.state.texture->pixels.empty(),
          "an in-flight draw keeps its decoded texture alive");
    check(have_first && first.state.texture_cpu != nullptr && first.state.texture_cpu->state().width == kSize,
          "an in-flight draw keeps its source texture alive");
    if (have_first && first.state.texture_cpu != nullptr) {
        std::vector<std::uint32_t> decoded;
        first.state.texture_cpu->decode(decoded);
        check(decoded.size() == static_cast<std::size_t>(kSize) * static_cast<std::size_t>(kSize),
              "an in-flight draw can still decode its source texture");
    }
}

} // namespace

int main() {
    through_triangle();
    indexed_and_textured();
    through_sprite_quad();
    scissor_and_gaps();
    blend_factor_table();
    clear_and_write_mask();
    alpha_test_decode();
    fog_decode();
    texwrap_decode();
    line_strip_emits();
    texfunc_decode();
    bounded_texture_retention();
    if (failures != 0) {
        std::cerr << "state_machine_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "state_machine_tests: OK\n";
    return 0;
}

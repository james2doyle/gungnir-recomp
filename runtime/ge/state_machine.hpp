#pragma once

// GE command state machine (T13b): turns a decoded display list into
// screen-space draws for the Vulkan backend. The mapping follows the public
// GE command set as implemented by psprecore (MIT) and PPSSPP; those are
// behavioral references only, no emulator code is linked.

#include "ge/display_list.hpp"
#include "ge/software.hpp"
#include "ge/texture.hpp"
#include "ge/vertex.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gungnir::ge {

// Copies guest bytes; false means the range is not readable.
using MemoryView = std::function<bool(std::uint32_t address, std::uint8_t *out, std::size_t size)>;

struct DrawState {
    int scissor_x0{};
    int scissor_y0{};
    int scissor_x1{};
    int scissor_y1{};
    bool depth_test{};
    bool depth_write{};
    CompareFunc depth_function{CompareFunc::Less};
    CullMode cull{CullMode::None};
    FrontFace front_face{FrontFace::Clockwise};
    BlendState blend{};
    bool alpha_test{};
    CompareFunc alpha_function{CompareFunc::Greater};
    std::uint8_t alpha_reference{};
    bool texture_enabled{};
    // Texture combine (TEXFUNC 0xC9 / TEXENVCOLOR 0xCA).
    TextureFunc tex_func{TextureFunc::Modulate};
    bool tex_rgba{};
    bool color_double{};
    std::uint32_t tex_env{};
    bool linear{};
    // TEXWRAP (0xC7) clamp flags: true means clamp to edge, false repeats.
    bool clamp_u{};
    bool clamp_v{};
    const TextureImage *texture{};
    const Texture *texture_cpu{};
    std::shared_ptr<const TextureImage> texture_storage;
    std::shared_ptr<const CpuTexture> texture_cpu_storage;
    std::uint64_t texture_key{};
    // Render-target awareness: the guest draw buffer (FBP 0x9C) this draw
    // targets, whether CLEAR mode (0xD3 bit 0) turns it into a clear, and
    // the color write mask (PMASK 0xE8/0xE9, or the clear bits).
    std::uint32_t draw_buffer{};
    bool clear{};
    // PMASK bits set = keep destination (PSP reset 0 = full write).
    std::uint32_t write_mask{};
    // Fog (0x1F/0xCD/0xCE/0xCF): enabled for transform-mode non-clear
    // draws, color is the raw 0xCF register (RGB, alpha ignored).
    bool fog_enabled{};
    std::uint32_t fog_color{};
};

struct Draw {
    Primitive primitive{Primitive::Invalid};
    std::vector<ScreenVertex> vertices;
    std::vector<std::uint16_t> indices;
    DrawState state;
};

// Keeps the GE register/matrix state and emits one Draw per PRIM packet.
// Lines and points are named gaps because the Vulkan mesh path is triangles;
// they are counted, never silently rendered as triangles.
class StateMachine {
public:
    using DrawSink = std::function<void(const Draw &)>;
    using GapSink = std::function<void(const std::string &)>;

    StateMachine();

    void reset();
    void set_draw_sink(DrawSink sink);
    void set_gap_sink(GapSink sink);
    void command(const Command &command, const MemoryView &memory);

    [[nodiscard]] const std::array<std::uint32_t, 256> &registers() const { return regs_; }
    [[nodiscard]] std::uint64_t draw_count() const { return draws_; }
    [[nodiscard]] std::uint64_t gap_count() const { return gaps_; }
    [[nodiscard]] std::size_t cached_texture_count() const;
    [[nodiscard]] std::size_t cached_texture_bytes() const;
    // Snapshot codec (T15c): registers, matrices, CLUT, addresses, layout and
    // counters. The texture cache is derived (re-decoded from guest RAM) and
    // is dropped on import; sinks are preserved.
    void export_snapshot(std::vector<std::uint8_t> &out) const;
    bool import_snapshot(std::span<const std::uint8_t> bytes, std::string &error);

private:
    void draw_primitive(const Command &command, const MemoryView &memory);
    [[nodiscard]] TransformState transform_state() const;
    [[nodiscard]] std::vector<ScreenVertex> transform_vertices(
        const std::vector<Vertex> &vertices, const TransformState &state) const;
    [[nodiscard]] const TextureImage *resolve_texture(const MemoryView &memory, DrawState &state);
    void gap(const std::string &message);
    [[nodiscard]] float reg_float(std::uint8_t opcode) const;
    [[nodiscard]] bool reg_on(std::uint8_t opcode) const;
    [[nodiscard]] std::uint32_t reg_param(std::uint8_t opcode) const;

    std::array<std::uint32_t, 256> regs_{};
    std::array<float, 12> world_{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 12> view_{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 12> texture_{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 16> projection_{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::array<std::array<float, 12>, 8> bones_{};
    std::array<std::uint8_t, 1024> clut_{};
    bool clut_valid_{};
    std::uint32_t vertex_address_{};
    std::uint32_t index_address_{};
    VertexLayout layout_{};
    int world_index_{};
    int view_index_{};
    int texture_index_{};
    int projection_index_{};
    int bone_index_{};
    std::uint64_t draws_{};
    std::uint64_t gaps_{};
    TextureCache textures_;
    DrawSink draw_sink_;
    GapSink gap_sink_;
};

} // namespace gungnir::ge

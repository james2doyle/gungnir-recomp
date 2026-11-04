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
#include <unordered_map>
#include <vector>

namespace gungnir::ge {

// Copies guest bytes; false means the range is not readable.
using MemoryView = std::function<bool(std::uint32_t address, std::uint8_t *out, std::size_t size)>;
// Zero-copy guest view for the texture fast path; empty span means unreadable.
using FastSource = std::function<std::span<const std::uint8_t>(std::uint32_t address, std::size_t size)>;

struct DrawState {
    // GE through mode: screen-space vertices, no transform. 2D/HUD draws are
    // through draws; they are never interpolation candidates.
    bool through{};
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
    StencilState stencil{};
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
    // targets, its pixel format (FBPPIXFORMAT 0xD2), whether CLEAR mode
    // (0xD3 bit 0) turns it into a clear, and the color write mask
    // (PMASK 0xE8/0xE9, or the clear bits).
    std::uint32_t draw_buffer{};
    std::uint32_t draw_format{};
    // Guest address + size backing this draw's texture (0 when untextured).
    // Used to detect render-to-texture reads and to invalidate stale
    // decoded textures when a draw target overlaps them.
    std::uint32_t texture_address{};
    std::size_t texture_size{};
    bool clear{};
    // 0xD3 bit 10 set keeps depth (does not clear it).
    bool clear_depth_write{true};
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
    using DrawSink = std::function<void(Draw &&)>;
    using GapSink = std::function<void(const std::string &)>;

    StateMachine();

    void reset();
    void set_draw_sink(DrawSink sink);
    void set_gap_sink(GapSink sink);
    void command(const Command &command, const MemoryView &memory);
    // Optional zero-copy source for texture resolves; when null every resolve
    // copies through MemoryView. end_frame drops the per-frame dedup map.
    void set_fast_source(FastSource source);
    void begin_list();
    void end_frame();
    [[nodiscard]] std::uint64_t texture_resolve_count() const;
    // Pipeline cache telemetry: how often the cached VertexPipeline was
    // rebuilt (transform state changed). Hits skip the state copy plus the
    // matrix precomputation in configure().
    [[nodiscard]] std::uint64_t pipeline_build_count() const { return pipeline_builds_; }

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
    // True when a State write to opcode feeds transform_state() (viewport,
    // texture-map, lighting, fog, cull regs). Matrix/bone uploads and
    // VertexType bump the generation separately.
    [[nodiscard]] static bool transform_affects(std::uint8_t opcode);
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
    FastSource fast_source_;
    std::uint64_t texture_resolves_{};
    // Per-draw scratch reused across draws (no semantic state; never
    // snapshotted). Grows to the largest draw seen, then stops allocating.
    std::vector<std::uint8_t> scratch_bytes_;
    std::vector<std::uint8_t> scratch_indices_;
    std::vector<Vertex> scratch_decoded_;
    std::vector<ClipVertex> scratch_clips_;
    std::vector<ClipVertex> scratch_polygon_;
    std::vector<ClipVertex> scratch_clip_tmp_;
    std::uint64_t list_epoch_{};
    // Cached vertex pipeline: transform_generation_ bumps on every write
    // that transform_state() reads (see transform_affects; the function body
    // is the spec — if it reads new state, add the opcodes here). A draw
    // whose generation matches the pipeline's skips the rebuild entirely.
    // Derived cache: never snapshotted; mismatched init forces first build.
    VertexPipeline pipeline_;
    std::uint64_t transform_generation_{1};
    std::uint64_t pipeline_generation_{};
    std::uint64_t pipeline_builds_{};
    // Per-frame validated dedup:cheap register key -> retained bytes from
    // this frame. Hits are byte-compared (never trusted blind); the map is
    // dropped by end_frame/reset/import.
    struct ResolveEntry {
        std::vector<std::uint8_t> clut;
        std::uint64_t content_key{};
        std::shared_ptr<const TextureImage> image;
        std::shared_ptr<CpuTexture> cpu;
        std::uint64_t validated_epoch{};
    };
    struct ResolveKey {
        std::uint32_t address{};
        std::uint32_t size{};
        std::uint32_t format{};
        std::uint32_t mode{};
        std::uint32_t extent{};
        std::uint32_t buffer_width{};
        std::uint32_t wrap{};
        std::uint32_t clut_format{};
        bool clut_valid{};
        bool operator==(const ResolveKey &) const = default;
    };
    struct ResolveKeyHash {
        std::size_t operator()(const ResolveKey &key) const noexcept {
            std::size_t hash = static_cast<std::size_t>(key.address);
            hash = hash * 1315423911u + key.size;
            hash = hash * 1315423911u + key.format;
            hash = hash * 1315423911u + key.mode;
            hash = hash * 1315423911u + key.extent;
            hash = hash * 1315423911u + key.buffer_width;
            hash = hash * 1315423911u + key.wrap;
            hash = hash * 1315423911u + key.clut_format;
            hash = hash * 1315423911u + (key.clut_valid ? 1u : 0u);
            return hash;
        }
    };
    std::unordered_map<ResolveKey, ResolveEntry, ResolveKeyHash> frame_textures_;
};

} // namespace gungnir::ge

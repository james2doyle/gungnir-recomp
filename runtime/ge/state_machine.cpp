#include "ge/state_machine.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>

namespace gungnir::ge {
namespace {

constexpr std::uint32_t kDisplayWidth = 480u;
constexpr std::uint32_t kDisplayHeight = 272u;

float f24(std::uint32_t param) {
    return std::bit_cast<float>(param << 8u);
}

Vec3 color24(std::uint32_t value) {
    return {static_cast<float>(value & 0xFFu) / 255.0f,
            static_cast<float>((value >> 8u) & 0xFFu) / 255.0f,
            static_cast<float>((value >> 16u) & 0xFFu) / 255.0f};
}

CompareFunc compare_from_ge(std::uint32_t value) {
    switch (value & 7u) {
    case 0u: return CompareFunc::Never;
    case 1u: return CompareFunc::Always;
    case 2u: return CompareFunc::Equal;
    case 3u: return CompareFunc::NotEqual;
    case 4u: return CompareFunc::Less;
    case 5u: return CompareFunc::LessEqual;
    case 6u: return CompareFunc::Greater;
    case 7u: return CompareFunc::GreaterEqual;
    default: return CompareFunc::Always;
    }
}

BlendFactor source_factor(std::uint32_t code) {
    switch (code & 0xFu) {
    case 0u: return BlendFactor::DstColor;
    case 1u: return BlendFactor::OneMinusDstColor;
    case 2u: return BlendFactor::SrcAlpha;
    case 3u: return BlendFactor::OneMinusSrcAlpha;
    case 4u: return BlendFactor::DstAlpha;
    case 5u: return BlendFactor::OneMinusDstAlpha;
    case 6u: return BlendFactor::DoubleSrcAlpha;
    case 7u: return BlendFactor::DoubleOneMinusSrcAlpha;
    case 8u: return BlendFactor::DoubleDstAlpha;
    case 9u: return BlendFactor::DoubleOneMinusDstAlpha;
    default: return BlendFactor::Fix;
    }
}

BlendFactor destination_factor(std::uint32_t code) {
    switch (code & 0xFu) {
    case 0u: return BlendFactor::SrcColor;
    case 1u: return BlendFactor::OneMinusSrcColor;
    case 2u: return BlendFactor::SrcAlpha;
    case 3u: return BlendFactor::OneMinusSrcAlpha;
    case 4u: return BlendFactor::DstAlpha;
    case 5u: return BlendFactor::OneMinusDstAlpha;
    case 6u: return BlendFactor::DoubleSrcAlpha;
    case 7u: return BlendFactor::DoubleOneMinusSrcAlpha;
    case 8u: return BlendFactor::DoubleDstAlpha;
    case 9u: return BlendFactor::DoubleOneMinusDstAlpha;
    default: return BlendFactor::Fix;
    }
}

BlendEquation blend_equation(std::uint32_t code) {
    switch (code & 7u) {
    case 1u: return BlendEquation::Subtract;
    case 2u: return BlendEquation::ReverseSubtract;
    case 3u: return BlendEquation::Min;
    case 4u: return BlendEquation::Max;
    default: return BlendEquation::Add;
    }
}

} // namespace

StateMachine::StateMachine() = default;

void StateMachine::reset() {
    regs_.fill(0u);
    world_ = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    view_ = world_;
    texture_ = world_;
    projection_ = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    bones_ = {};
    clut_.fill(0u);
    clut_valid_ = false;
    vertex_address_ = 0u;
    index_address_ = 0u;
    layout_ = VertexLayout{};
    world_index_ = view_index_ = texture_index_ = projection_index_ = bone_index_ = 0;
    draws_ = 0u;
    gaps_ = 0u;
    textures_.clear();
}

void StateMachine::set_draw_sink(DrawSink sink) {
    draw_sink_ = std::move(sink);
}

void StateMachine::set_gap_sink(GapSink sink) {
    gap_sink_ = std::move(sink);
}

std::size_t StateMachine::cached_texture_count() const { return textures_.size(); }

std::size_t StateMachine::cached_texture_bytes() const { return textures_.byte_size(); }

std::uint32_t StateMachine::reg_param(std::uint8_t opcode) const {
    return regs_[opcode] & 0xFFFFFFu;
}

float StateMachine::reg_float(std::uint8_t opcode) const {
    return f24(reg_param(opcode));
}

bool StateMachine::reg_on(std::uint8_t opcode) const {
    return (regs_[opcode] & 1u) != 0u;
}

void StateMachine::gap(const std::string &message) {
    ++gaps_;
    if (gap_sink_) gap_sink_(message);
}

void StateMachine::command(const Command &command, const MemoryView &memory) {
    switch (command.kind) {
    case CommandKind::VertexAddress:
        vertex_address_ = command.address;
        break;
    case CommandKind::IndexAddress:
        index_address_ = command.address;
        break;
    case CommandKind::VertexType:
        layout_ = command.vertex_layout;
        break;
    case CommandKind::Primitive:
        draw_primitive(command, memory);
        break;
    case CommandKind::State:
        regs_[command.opcode] = command.argument;
        if (command.opcode == 0xC4u) {
            const std::uint32_t address = ((reg_param(0xB1u) & 0x0F0000u) << 8u) |
                (reg_param(0xB0u) & 0xFFFFF0u);
            const std::size_t bytes = std::min<std::size_t>((command.argument & 0x3Fu) * 32u, clut_.size());
            if (bytes != 0u && memory && memory(address, clut_.data(), bytes)) {
                clut_valid_ = true;
            } else if (bytes != 0u) {
                gap("clut_load_unreadable");
            }
        }
        break;
    case CommandKind::Base:
    case CommandKind::Offset:
    case CommandKind::Origin:
        break;
    default:
        break;
    }

    switch (command.opcode) {
    case 0x3Au: world_index_ = static_cast<int>(command.argument & 15u); break;
    case 0x3Bu:
        if (world_index_ >= 0 && world_index_ < 12) world_[world_index_++] = f24(command.argument);
        else gap("world_matrix_index");
        break;
    case 0x3Cu: view_index_ = static_cast<int>(command.argument & 15u); break;
    case 0x3Du:
        if (view_index_ >= 0 && view_index_ < 12) view_[view_index_++] = f24(command.argument);
        else gap("view_matrix_index");
        break;
    case 0x3Eu: projection_index_ = static_cast<int>(command.argument & 15u); break;
    case 0x3Fu:
        if (projection_index_ >= 0 && projection_index_ < 16)
            projection_[projection_index_++] = f24(command.argument);
        else gap("projection_matrix_index");
        break;
    case 0x40u: texture_index_ = static_cast<int>(command.argument & 15u); break;
    case 0x41u:
        if (texture_index_ >= 0 && texture_index_ < 12) texture_[texture_index_++] = f24(command.argument);
        else gap("texture_matrix_index");
        break;
    case 0x2Au: bone_index_ = static_cast<int>(command.argument & 127u); break;
    case 0x2Bu:
        if (bone_index_ >= 0 && bone_index_ < 96) {
            bones_[static_cast<std::size_t>(bone_index_ / 12)][static_cast<std::size_t>(bone_index_ % 12)] =
                f24(command.argument);
            ++bone_index_;
        } else {
            gap("bone_matrix_index");
        }
        break;
    default:
        break;
    }
}

TransformState StateMachine::transform_state() const {
    TransformState state;
    state.layout = layout_;
    state.through = layout_.through;
    for (std::size_t i = 0; i < 12u; ++i) {
        state.world.m[i] = world_[i];
        state.view.m[i] = view_[i];
        state.texture.m[i] = texture_[i];
    }
    for (std::size_t i = 0; i < 16u; ++i) state.projection.m[i] = projection_[i];
    state.viewport.scale = {reg_float(0x42u), reg_float(0x43u), reg_float(0x44u)};
    state.viewport.center = {reg_float(0x45u), reg_float(0x46u), reg_float(0x47u)};
    state.viewport.offset_x = static_cast<float>(reg_param(0x4Cu) & 0xFFFFu) / 16.0f;
    state.viewport.offset_y = static_cast<float>(reg_param(0x4Du) & 0xFFFFu) / 16.0f;
    state.texture_map.enabled = reg_on(0x1Eu);
    state.texture_map.scale = {reg_float(0x48u), reg_float(0x49u)};
    state.texture_map.offset = {reg_float(0x4Au), reg_float(0x4Bu)};
    const std::uint32_t map = reg_param(0xC0u);
    state.texture_map.mode = (map & 3u) == 1u ? TextureMode::Matrix : TextureMode::Uv;
    state.texture_map.source = static_cast<TextureSource>(std::min<std::uint32_t>((map >> 8u) & 3u, 3u));
    const std::uint32_t tex_size = reg_param(0xB8u);
    state.texture_map.width = static_cast<float>(1u << std::min<std::uint32_t>(tex_size & 15u, 10u));
    state.texture_map.height = static_cast<float>(1u << std::min<std::uint32_t>((tex_size >> 8u) & 15u, 10u));

    state.lighting.enabled = reg_on(0x17u) && !layout_.through;
    if (state.lighting.enabled) {
        state.lighting.reverse_normal = reg_on(0x51u);
        state.lighting.separate_specular = (reg_param(0x5Eu) & 1u) != 0u;
        Material &material = state.lighting.material;
        material.emissive = color24(reg_param(0x54u));
        material.ambient = color24(reg_param(0x55u));
        material.diffuse = color24(reg_param(0x56u));
        material.specular = color24(reg_param(0x57u));
        material.alpha = static_cast<float>(reg_param(0x58u) & 0xFFu) / 255.0f;
        material.specular_coefficient = reg_float(0x5Bu);
        material.update = static_cast<std::uint8_t>(reg_param(0x53u) & 7u);
        state.lighting.ambient = color24(reg_param(0x5Cu));
        state.lighting.ambient_alpha = static_cast<float>(reg_param(0x5Du) & 0xFFu) / 255.0f;
        for (int i = 0; i < 4; ++i) {
            Light &light = state.lighting.lights[static_cast<std::size_t>(i)];
            light.enabled = reg_on(static_cast<std::uint8_t>(0x18 + i));
            if (!light.enabled) continue;
            const std::uint32_t type = reg_param(static_cast<std::uint8_t>(0x5F + i));
            light.kind = static_cast<LightKind>(type & 3u);
            light.type = static_cast<LightType>((type >> 8u) & 3u);
            const auto read3 = [this](std::uint8_t base) {
                return Vec3{reg_float(base), reg_float(static_cast<std::uint8_t>(base + 1u)),
                            reg_float(static_cast<std::uint8_t>(base + 2u))};
            };
            light.position = read3(static_cast<std::uint8_t>(0x63 + 3 * i));
            light.direction = read3(static_cast<std::uint8_t>(0x6F + 3 * i));
            light.attenuation = read3(static_cast<std::uint8_t>(0x7B + 3 * i));
            light.exponent = reg_float(static_cast<std::uint8_t>(0x87 + i));
            light.cutoff = reg_float(static_cast<std::uint8_t>(0x8B + i));
            light.ambient = color24(reg_param(static_cast<std::uint8_t>(0x8F + 3 * i)));
            light.diffuse = color24(reg_param(static_cast<std::uint8_t>(0x8F + 3 * i + 1)));
            light.specular = color24(reg_param(static_cast<std::uint8_t>(0x8F + 3 * i + 2)));
        }
    }
    for (std::size_t i = 0; i < bones_.size(); ++i) {
        for (std::size_t j = 0; j < 12u; ++j) state.bones[i].m[j] = bones_[i][j];
    }
    if (reg_on(0x1Du) && !layout_.through) {
        state.cull = CullMode::Back;
        state.front_face = (reg_param(0x9Bu) & 1u) != 0u ? FrontFace::CounterClockwise : FrontFace::Clockwise;
    }
    return state;
}

std::vector<ScreenVertex> StateMachine::transform_vertices(
    const std::vector<Vertex> &vertices, const TransformState &state) const {
    VertexPipeline pipeline(state);
    std::vector<ScreenVertex> out;
    out.reserve(vertices.size());
    for (const Vertex &vertex : vertices) out.push_back(pipeline.project(pipeline.transform(vertex)));
    return out;
}

const TextureImage *StateMachine::resolve_texture(const MemoryView &memory, DrawState &state) {
    if (!reg_on(0x1Eu) || (reg_on(0xD3u) && (reg_param(0xD3u) & 1u) != 0u)) return nullptr;
    if ((reg_param(0xC9u) & 7u) != 0u) gap("tex_func_not_modulate");
    if ((reg_param(0xC0u) & 3u) != 0u) gap("tex_map_mode");
    const std::uint32_t format = reg_param(0xC3u) & 15u;
    if (format > 7u) {
        gap("texture_format_unsupported");
        return nullptr;
    }
    const std::uint32_t mode = reg_param(0xC2u);
    const std::uint32_t levels = (mode >> 16u) & 7u;
    if (levels != 0u) gap("texture_mip_levels");
    const std::uint32_t size = reg_param(0xB8u);
    const int width = 1 << std::min<std::uint32_t>(size & 15u, 10u);
    const int height = 1 << std::min<std::uint32_t>((size >> 8u) & 15u, 10u);
    const std::uint32_t buffer_width = reg_param(0xA8u) & 0x7FFu;
    if (buffer_width == 0u || width == 0 || height == 0) {
        gap("texture_extent_zero");
        return nullptr;
    }
    const std::uint32_t address = ((reg_param(0xA8u) & 0x0F0000u) << 8u) |
        (reg_param(0xA0u) & 0xFFFFF0u);
    static constexpr std::uint32_t kBits[8] = {16u, 16u, 16u, 32u, 4u, 8u, 16u, 32u};
    const std::size_t row_bytes = static_cast<std::size_t>(buffer_width) * kBits[format] / 8u;
    const std::size_t data_size = row_bytes * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> data(data_size, 0u);
    if (data_size != 0u && !memory(address, data.data(), data_size)) {
        gap("texture_unreadable");
        return nullptr;
    }

    std::array<std::uint8_t, 1024> clut{};
    bool has_clut = false;
    if (format >= 4u) {
        if (clut_valid_) {
            clut = clut_;
            has_clut = true;
        } else {
            const std::uint32_t clut_address = ((reg_param(0xB1u) & 0x0F0000u) << 8u) |
                (reg_param(0xB0u) & 0xFFFFF0u);
            if (clut_address != 0u && memory(clut_address, clut.data(), clut.size()))
                has_clut = true;
            else
                gap("clut_unreadable");
        }
    }

    TextureState texture_state;
    texture_state.format = static_cast<TextureFormat>(format);
    texture_state.swizzled = (mode & 1u) != 0u;
    texture_state.width = width;
    texture_state.height = height;
    texture_state.buffer_width = static_cast<int>(buffer_width);
    texture_state.clamp_u = (reg_param(0xC7u) & 1u) != 0u;
    texture_state.clamp_v = ((reg_param(0xC7u) >> 8u) & 1u) != 0u;
    texture_state.linear = (reg_param(0xC6u) & 1u) != 0u;
    texture_state.clut_format = reg_param(0xC5u);
    auto cpu = std::make_shared<CpuTexture>();
    cpu->data = std::move(data);
    cpu->clut.assign(clut.begin(), clut.end());
    cpu->texture.emplace(texture_state, std::span<const std::uint8_t>(cpu->data),
                         std::span<const std::uint8_t>(cpu->clut));
    const Texture &texture = *cpu->texture;

    const std::uint64_t sampler = (texture_state.linear ? 1ull : 0ull) |
        (texture_state.clamp_u ? 2ull : 0ull) | (texture_state.clamp_v ? 4ull : 0ull);
    const std::uint64_t key = texture.content_key() ^ (sampler * 0x9E3779B97F4A7C15ull);
    const std::shared_ptr<const TextureImage> image = textures_.get_shared(key, texture);
    state.texture_enabled = true;
    state.texture = image.get();
    state.texture_cpu = &texture;
    state.texture_storage = image;
    state.texture_cpu_storage = cpu;
    state.texture_key = key;
    state.linear = texture_state.linear;
    state.wrap_u = texture_state.clamp_u;
    state.wrap_v = texture_state.clamp_v;
    return state.texture;
}

void StateMachine::draw_primitive(const Command &command, const MemoryView &memory) {
    const std::uint32_t count = command.count;
    if (count == 0u) return;
    const std::size_t stride = layout_.stride;
    if (stride == 0u) {
        gap("vertex_stride_zero");
        return;
    }
    const int index_size = layout_.index_size;
    std::vector<std::uint8_t> vertices(static_cast<std::size_t>(count) * stride);
    if (index_size != 0) {
        std::vector<std::uint8_t> indices(static_cast<std::size_t>(count) * static_cast<std::size_t>(index_size));
        if (!memory(index_address_, indices.data(), indices.size())) {
            gap("index_unreadable");
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t index = 0;
            if (index_size == 1) index = indices[i];
            else if (index_size == 2) std::memcpy(&index, indices.data() + i * 2u, 2u);
            else std::memcpy(&index, indices.data() + i * 4u, 4u);
            if (!memory(vertex_address_ + index * static_cast<std::uint32_t>(stride),
                        vertices.data() + static_cast<std::size_t>(i) * stride, stride)) {
                gap("vertex_unreadable");
                return;
            }
        }
        index_address_ += count * static_cast<std::uint32_t>(index_size);
    } else {
        if (!memory(vertex_address_, vertices.data(), vertices.size())) {
            gap("vertex_unreadable");
            return;
        }
        vertex_address_ += count * static_cast<std::uint32_t>(stride);
    }

    std::array<float, 8> morph{};
    for (std::size_t i = 0; i < morph.size(); ++i)
        morph[i] = reg_float(static_cast<std::uint8_t>(0x2Cu + i));
    std::vector<Vertex> decoded(count);
    for (std::uint32_t i = 0; i < count; ++i)
        decoded[i] = decode_vertex(layout_, vertices.data() + static_cast<std::size_t>(i) * stride, morph.data());

    const TransformState state = transform_state();
    VertexPipeline pipeline(state);
    std::vector<ClipVertex> clips(count);
    for (std::uint32_t i = 0; i < count; ++i) clips[i] = pipeline.transform(decoded[i]);

    Draw draw;
    draw.primitive = command.primitive;
    const std::uint32_t scissor1 = reg_param(0xD4u);
    const std::uint32_t scissor2 = reg_param(0xD5u);
    draw.state.scissor_x0 = static_cast<int>(scissor1 & 0x3FFu);
    draw.state.scissor_y0 = static_cast<int>((scissor1 >> 10u) & 0x3FFu);
    draw.state.scissor_x1 = static_cast<int>(scissor2 & 0x3FFu) + 1;
    draw.state.scissor_y1 = static_cast<int>((scissor2 >> 10u) & 0x3FFu) + 1;
    // A zero SCISSOR2 is the reset state, not a one-pixel window.
    if (scissor2 == 0u || draw.state.scissor_x1 <= draw.state.scissor_x0 ||
        draw.state.scissor_y1 <= draw.state.scissor_y0) {
        draw.state.scissor_x0 = 0;
        draw.state.scissor_y0 = 0;
        draw.state.scissor_x1 = static_cast<int>(kDisplayWidth);
        draw.state.scissor_y1 = static_cast<int>(kDisplayHeight);
    }
    draw.state.depth_test = reg_on(0x23u);
    draw.state.depth_write = !reg_on(0xE7u);
    draw.state.depth_function = compare_from_ge(reg_param(0xDEu));
    draw.state.cull = state.cull;
    draw.state.front_face = state.front_face;
    draw.state.draw_buffer = reg_param(0x9Cu) & 0x00FFFFFFu;
    draw.state.clear = (reg_param(0xD3u) & 1u) != 0u;
    if (draw.state.clear) {
        const std::uint32_t clear = reg_param(0xD3u);
        draw.state.write_mask = ((clear & 0x100u) != 0u ? 0u : 0x00FFFFFFu) |
                                ((clear & 0x200u) != 0u ? 0u : 0xFF000000u);
    } else {
        draw.state.write_mask =
            (reg_param(0xE8u) & 0x00FFFFFFu) | ((reg_param(0xE9u) & 0xFFu) << 24u);
    }
    if (reg_on(0x21u) && !draw.state.clear) {
        const std::uint32_t mode = reg_param(0xDFu);
        draw.state.blend.enabled = true;
        draw.state.blend.source = source_factor(mode & 0xFu);
        draw.state.blend.destination = destination_factor((mode >> 4u) & 0xFu);
        draw.state.blend.equation = blend_equation((mode >> 8u) & 7u);
        draw.state.blend.fixed_source = reg_param(0xE0u);
        draw.state.blend.fixed_destination = reg_param(0xE1u);
    }
    draw.state.alpha_test = reg_on(0x22u) && !draw.state.clear;
    if (draw.state.alpha_test) {
        const std::uint32_t alpha = reg_param(0xDBu);
        draw.state.alpha_function = compare_from_ge(alpha & 7u);
        draw.state.alpha_reference = static_cast<std::uint8_t>((alpha >> 8u) & 0xFFu);
        // Alpha mask (bits 16-23) not applied yet (named gap if ever observed).
    }
    if (reg_on(0x17u) && !layout_.through && (reg_param(0x50u) & 1u) == 0u) gap("flat_shading_unsupported");
    resolve_texture(memory, draw.state);

    const auto append_screen = [&](const ClipVertex &clip) -> std::uint16_t {
        const ScreenVertex screen = pipeline.project(clip);
        if (draw.vertices.size() >= 0xFFFFu) return 0xFFFFu;
        draw.vertices.push_back(screen);
        return static_cast<std::uint16_t>(draw.vertices.size() - 1u);
    };
    const auto emit_triangle = [&](const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
        if (!layout_.through && (a.clip[3] <= 0.0f || b.clip[3] <= 0.0f || c.clip[3] <= 0.0f)) {
            gap("clip_w_nonpositive");
            return;
        }
        std::vector<ClipVertex> polygon;
        if (layout_.through) {
            polygon = {a, b, c};
        } else {
            polygon = clip_triangle(a, b, c);
        }
        if (polygon.size() < 3u) return;
        const auto base = static_cast<std::uint16_t>(draw.vertices.size());
        for (const ClipVertex &vertex : polygon) append_screen(vertex);
        for (std::size_t i = 1u; i + 1u < polygon.size(); ++i) {
            const ScreenVertex &first = draw.vertices[base];
            const ScreenVertex &second = draw.vertices[base + i];
            const ScreenVertex &third = draw.vertices[base + i + 1u];
            if (state.cull != CullMode::None &&
                VertexPipeline::cull_triangle(first, second, third, state.cull, state.front_face))
                continue;
            draw.indices.push_back(base);
            draw.indices.push_back(static_cast<std::uint16_t>(base + i));
            draw.indices.push_back(static_cast<std::uint16_t>(base + i + 1u));
        }
    };

    switch (command.primitive) {
    case Primitive::Triangles:
        for (std::uint32_t i = 0; i + 2u < count; i += 3u) emit_triangle(clips[i], clips[i + 1u], clips[i + 2u]);
        break;
    case Primitive::TriangleStrip:
        for (std::uint32_t i = 0; i + 2u < count; ++i) {
            if ((i & 1u) != 0u) emit_triangle(clips[i + 1u], clips[i], clips[i + 2u]);
            else emit_triangle(clips[i], clips[i + 1u], clips[i + 2u]);
        }
        break;
    case Primitive::TriangleFan:
        for (std::uint32_t i = 1u; i + 1u < count; ++i) emit_triangle(clips[0], clips[i], clips[i + 1u]);
        break;
    case Primitive::Sprites: {
        for (std::uint32_t i = 0; i + 1u < count; i += 2u) {
            if (!layout_.through && (clips[i].clip[3] <= 0.0f || clips[i + 1u].clip[3] <= 0.0f)) {
                gap("sprite_w_nonpositive");
                continue;
            }
            const ScreenVertex top_left = pipeline.project(clips[i]);
            const ScreenVertex bottom_right = pipeline.project(clips[i + 1u]);
            const float q0 = clips[i].texcoord[2] != 0.0f ? clips[i].texcoord[2] : 1.0f;
            const float q1 = clips[i + 1u].texcoord[2] != 0.0f ? clips[i + 1u].texcoord[2] : 1.0f;
            const float u0 = top_left.texcoord[0] / q0;
            const float v0 = top_left.texcoord[1] / q0;
            const float u1 = bottom_right.texcoord[0] / q1;
            const float v1 = bottom_right.texcoord[1] / q1;
            ScreenVertex tl = top_left;
            ScreenVertex tr = bottom_right;
            ScreenVertex bl = top_left;
            ScreenVertex br = bottom_right;
            tr.y = top_left.y;
            tr.texcoord = {u1, v0, 1.0f};
            bl.x = top_left.x;
            bl.y = bottom_right.y;
            bl.texcoord = {u0, v1, 1.0f};
            br.texcoord = {u1, v1, 1.0f};
            tl.texcoord = {u0, v0, 1.0f};
            tl.color = tr.color = bl.color = br.color = bottom_right.color;
            tl.z = tr.z = bl.z = br.z = bottom_right.z;
            tl.inv_w = tr.inv_w = bl.inv_w = br.inv_w = 1.0f;
            const auto base = static_cast<std::uint16_t>(draw.vertices.size());
            draw.vertices.insert(draw.vertices.end(), {tl, tr, bl, br});
            draw.indices.insert(draw.indices.end(),
                {base, static_cast<std::uint16_t>(base + 1u), static_cast<std::uint16_t>(base + 2u),
                 static_cast<std::uint16_t>(base + 2u), static_cast<std::uint16_t>(base + 1u),
                 static_cast<std::uint16_t>(base + 3u)});
        }
        break;
    }
    case Primitive::Points:
    case Primitive::Lines:
    case Primitive::LineStrip:
        gap(std::string("primitive_unsupported:") + primitive_name(command.primitive));
        break;
    case Primitive::Invalid:
        gap("primitive_invalid");
        break;
    }

    if (!draw.vertices.empty() && !draw.indices.empty()) {
        ++draws_;
        if (draw_sink_) draw_sink_(draw);
    }
}

namespace {

void mput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void mput_u64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFu));
}

void mput_f32(std::vector<std::uint8_t> &out, float value) {
    mput_u32(out, std::bit_cast<std::uint32_t>(value));
}

struct MReader {
    const std::uint8_t *p{};
    std::size_t left{};
    bool ok{true};

    [[nodiscard]] std::uint32_t u32() {
        if (left < 4u) {
            ok = false;
            return 0u;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
        p += 4;
        left -= 4;
        return v;
    }

    [[nodiscard]] std::uint64_t u64() {
        if (left < 8u) {
            ok = false;
            return 0u;
        }
        std::uint64_t v = 0u;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8);
        p += 8;
        left -= 8;
        return v;
    }

    [[nodiscard]] std::uint8_t u8() {
        if (left < 1u) {
            ok = false;
            return 0u;
        }
        const std::uint8_t v = *p;
        ++p;
        --left;
        return v;
    }

    [[nodiscard]] std::uint16_t u16() {
        if (left < 2u) {
            ok = false;
            return 0u;
        }
        const std::uint16_t v =
            static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8u));
        p += 2;
        left -= 2;
        return v;
    }

    [[nodiscard]] float f32() { return std::bit_cast<float>(u32()); }
};

void mput_layout(std::vector<std::uint8_t> &out, const VertexLayout &layout) {
    mput_u32(out, layout.raw);
    out.push_back(layout.texcoord);
    out.push_back(layout.color);
    out.push_back(layout.normal);
    out.push_back(layout.position);
    out.push_back(layout.weight);
    out.push_back(layout.index);
    out.push_back(layout.weight_count);
    out.push_back(layout.morph_count);
    out.push_back(layout.through ? 1u : 0u);
    out.push_back(layout.index_size);
    mput_u32(out, (static_cast<std::uint32_t>(layout.weight_offset) << 16u) | layout.stride);
    const std::uint16_t offsets[5] = {layout.texcoord_offset, layout.color_offset, layout.normal_offset,
        layout.position_offset, layout.weight_offset};
    for (std::uint16_t offset : offsets) {
        out.push_back(static_cast<std::uint8_t>(offset & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((offset >> 8u) & 0xFFu));
    }
}

bool mget_layout(MReader &in, VertexLayout &layout) {
    layout.raw = in.u32();
    layout.texcoord = in.u8();
    layout.color = in.u8();
    layout.normal = in.u8();
    layout.position = in.u8();
    layout.weight = in.u8();
    layout.index = in.u8();
    layout.weight_count = in.u8();
    layout.morph_count = in.u8();
    const std::uint8_t through = in.u8();
    layout.index_size = in.u8();
    const std::uint32_t packed = in.u32();
    layout.stride = static_cast<std::uint16_t>(packed & 0xFFFFu);
    const std::uint16_t offsets[5] = {in.u16(), in.u16(), in.u16(), in.u16(), in.u16()};
    if (!in.ok || through > 1u) return false;
    layout.through = through != 0u;
    layout.texcoord_offset = offsets[0];
    layout.color_offset = offsets[1];
    layout.normal_offset = offsets[2];
    layout.position_offset = offsets[3];
    layout.weight_offset = offsets[4];
    if (layout.weight_offset != ((packed >> 16u) & 0xFFFFu)) return false;
    return true;
}

} // namespace

void StateMachine::export_snapshot(std::vector<std::uint8_t> &out) const {
    for (std::uint32_t reg : regs_) mput_u32(out, reg);
    for (float lane : world_) mput_f32(out, lane);
    for (float lane : view_) mput_f32(out, lane);
    for (float lane : texture_) mput_f32(out, lane);
    for (float lane : projection_) mput_f32(out, lane);
    for (const auto &bone : bones_)
        for (float lane : bone) mput_f32(out, lane);
    out.insert(out.end(), clut_.begin(), clut_.end());
    out.push_back(clut_valid_ ? 1u : 0u);
    mput_u32(out, vertex_address_);
    mput_u32(out, index_address_);
    mput_layout(out, layout_);
    mput_u32(out, static_cast<std::uint32_t>(world_index_));
    mput_u32(out, static_cast<std::uint32_t>(view_index_));
    mput_u32(out, static_cast<std::uint32_t>(texture_index_));
    mput_u32(out, static_cast<std::uint32_t>(projection_index_));
    mput_u32(out, static_cast<std::uint32_t>(bone_index_));
    mput_u64(out, draws_);
    mput_u64(out, gaps_);
}

bool StateMachine::import_snapshot(std::span<const std::uint8_t> bytes, std::string &error) {
    MReader in{bytes.data(), bytes.size()};
    std::array<std::uint32_t, 256> regs{};
    for (std::uint32_t &reg : regs) reg = in.u32();
    std::array<float, 12> world{};
    for (float &lane : world) lane = in.f32();
    std::array<float, 12> view{};
    for (float &lane : view) lane = in.f32();
    std::array<float, 12> texture{};
    for (float &lane : texture) lane = in.f32();
    std::array<float, 16> projection{};
    for (float &lane : projection) lane = in.f32();
    std::array<std::array<float, 12>, 8> bones{};
    for (auto &bone : bones)
        for (float &lane : bone) lane = in.f32();
    if (!in.ok) {
        error = "snapshot_truncated: ge state matrices truncated";
        return false;
    }
    if (in.left < 1024u + 1u + 8u) {
        error = "snapshot_truncated: ge clut truncated";
        return false;
    }
    std::array<std::uint8_t, 1024> clut{};
    std::copy_n(in.p, 1024u, clut.begin());
    in.p += 1024u;
    in.left -= 1024u;
    const std::uint8_t clut_valid = in.u8();
    const std::uint32_t vertex_address = in.u32();
    const std::uint32_t index_address = in.u32();
    VertexLayout layout{};
    if (!mget_layout(in, layout)) {
        error = "snapshot_bad_length: ge vertex layout malformed";
        return false;
    }
    const int world_index = static_cast<int>(in.u32());
    const int view_index = static_cast<int>(in.u32());
    const int texture_index = static_cast<int>(in.u32());
    const int projection_index = static_cast<int>(in.u32());
    const int bone_index = static_cast<int>(in.u32());
    const std::uint64_t draws = in.u64();
    const std::uint64_t gaps = in.u64();
    if (!in.ok || in.left != 0u || clut_valid > 1u) {
        error = "snapshot_truncated: ge state trailing bytes";
        return false;
    }
    regs_ = regs;
    world_ = world;
    view_ = view;
    texture_ = texture;
    projection_ = projection;
    bones_ = bones;
    clut_ = clut;
    clut_valid_ = clut_valid != 0u;
    vertex_address_ = vertex_address;
    index_address_ = index_address;
    layout_ = layout;
    world_index_ = world_index;
    view_index_ = view_index;
    texture_index_ = texture_index;
    projection_index_ = projection_index;
    bone_index_ = bone_index;
    draws_ = draws;
    gaps_ = gaps;
    textures_.clear();
    error.clear();
    return true;
}

} // namespace gungnir::ge

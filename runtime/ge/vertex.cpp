#include "ge/vertex.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gungnir::ge {
namespace {

std::uint16_t read_u16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0]) | static_cast<std::uint16_t>(data[1]) << 8u;
}

std::uint32_t read_u32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) | static_cast<std::uint32_t>(data[1]) << 8u |
           static_cast<std::uint32_t>(data[2]) << 16u | static_cast<std::uint32_t>(data[3]) << 24u;
}

float read_float(const std::uint8_t *data) {
    const std::uint32_t bits = read_u32(data);
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

float component(const std::uint8_t *data, int kind, int index, bool is_signed, float scale) {
    switch (kind) {
    case 1: {
        const std::uint8_t raw = data[index];
        return (is_signed ? static_cast<float>(static_cast<std::int8_t>(raw)) : static_cast<float>(raw)) * scale;
    }
    case 2: {
        const std::uint16_t raw = read_u16(data + index * 2);
        return (is_signed ? static_cast<float>(static_cast<std::int16_t>(raw)) : static_cast<float>(raw)) * scale;
    }
    case 3:
        return read_float(data + index * 4);
    default:
        return 0.0f;
    }
}

void decode_color(const std::uint8_t *data, int kind, std::array<float, 4> &out) {
    const std::uint32_t packed = kind == 7 ? read_u32(data) : read_u16(data);
    switch (kind) {
    case 4:
        out[0] = static_cast<float>(packed & 31u) / 31.0f;
        out[1] = static_cast<float>((packed >> 5u) & 63u) / 63.0f;
        out[2] = static_cast<float>((packed >> 11u) & 31u) / 31.0f;
        out[3] = 1.0f;
        break;
    case 5:
        out[0] = static_cast<float>(packed & 31u) / 31.0f;
        out[1] = static_cast<float>((packed >> 5u) & 31u) / 31.0f;
        out[2] = static_cast<float>((packed >> 10u) & 31u) / 31.0f;
        out[3] = static_cast<float>((packed >> 15u) & 1u);
        break;
    case 6:
        for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((packed >> (i * 4)) & 15u) / 15.0f;
        break;
    default:
        for (int i = 0; i < 4; ++i) out[i] = static_cast<float>((packed >> (i * 8)) & 255u) / 255.0f;
        break;
    }
}

float plane_distance_near(const ClipVertex &vertex) {
    return vertex.clip[2] + vertex.clip[3];
}

float plane_distance_far(const ClipVertex &vertex) {
    return vertex.clip[3] - vertex.clip[2];
}

ClipVertex lerp(const ClipVertex &a, const ClipVertex &b, float t) {
    ClipVertex out;
    for (int i = 0; i < 4; ++i) out.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
    for (int i = 0; i < 3; ++i) out.texcoord[i] = a.texcoord[i] + (b.texcoord[i] - a.texcoord[i]) * t;
    for (int i = 0; i < 4; ++i) out.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
    for (int i = 0; i < 3; ++i) out.specular[i] = a.specular[i] + (b.specular[i] - a.specular[i]) * t;
    out.fog = a.fog + (b.fog - a.fog) * t;
    return out;
}

void clip_plane_into(std::vector<ClipVertex> &out, const std::vector<ClipVertex> &polygon,
                       float (*distance)(const ClipVertex &)) {
    out.clear();
    if (polygon.empty()) return;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const ClipVertex &current = polygon[i];
        const ClipVertex &next = polygon[(i + 1u) % polygon.size()];
        const float current_distance = distance(current);
        const float next_distance = distance(next);
        const bool current_inside = current_distance >= 0.0f;
        const bool next_inside = next_distance >= 0.0f;
        if (current_inside && next_inside) {
            out.push_back(next);
        } else if (current_inside && !next_inside) {
            const float t = current_distance / (current_distance - next_distance);
            out.push_back(lerp(current, next, t));
        } else if (!current_inside && next_inside) {
            const float t = current_distance / (current_distance - next_distance);
            out.push_back(lerp(current, next, t));
            out.push_back(next);
        }
    }
}

std::vector<ClipVertex> clip_plane(const std::vector<ClipVertex> &polygon, float (*distance)(const ClipVertex &)) {
    std::vector<ClipVertex> out;
    clip_plane_into(out, polygon, distance);
    return out;
}


Vec3 material_color(const Vertex &vertex, const Material &material, std::uint8_t bit, float &alpha) {
    if (vertex.color[0] >= 0.0f && (material.update & bit) != 0u) {
        if (bit == 1u) alpha = vertex.color[3];
        return {vertex.color[0], vertex.color[1], vertex.color[2]};
    }
    return bit == 1u ? material.ambient : bit == 2u ? material.diffuse : material.specular;
}

} // namespace

Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 operator*(Vec3 a, float scalar) {
    return {a.x * scalar, a.y * scalar, a.z * scalar};
}

Vec3 multiply(Vec3 a, Vec3 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalize(Vec3 value) {
    const float length = std::sqrt(dot(value, value));
    return length > 0.0f ? value * (1.0f / length) : value;
}

Vec3 Mat43::transform(Vec3 value, float w) const {
    return {m[0] * value.x + m[3] * value.y + m[6] * value.z + m[9] * w,
            m[1] * value.x + m[4] * value.y + m[7] * value.z + m[10] * w,
            m[2] * value.x + m[5] * value.y + m[8] * value.z + m[11] * w};
}

Vec4 Mat44::transform(Vec4 value) const {
    return {m[0] * value.x + m[4] * value.y + m[8] * value.z + m[12] * value.w,
            m[1] * value.x + m[5] * value.y + m[9] * value.z + m[13] * value.w,
            m[2] * value.x + m[6] * value.y + m[10] * value.z + m[14] * value.w,
            m[3] * value.x + m[7] * value.y + m[11] * value.z + m[15] * value.w};
}

Mat43 operator*(const Mat43 &a, const Mat43 &b) {
    Mat43 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 3; ++row) {
            out.m[column * 3 + row] = a.m[row] * b.m[column * 3] + a.m[3 + row] * b.m[column * 3 + 1] +
                                      a.m[6 + row] * b.m[column * 3 + 2] +
                                      (column == 3 ? a.m[9 + row] : 0.0f);
        }
    }
    return out;
}

Mat44 operator*(const Mat44 &a, const Mat43 &b) {
    Mat44 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out.m[column * 4 + row] = a.m[row] * b.m[column * 3] + a.m[4 + row] * b.m[column * 3 + 1] +
                                      a.m[8 + row] * b.m[column * 3 + 2] +
                                      (column == 3 ? a.m[12 + row] : 0.0f);
        }
    }
    return out;
}

Mat44 operator*(const Mat44 &a, const Mat44 &b) {
    Mat44 out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            out.m[column * 4 + row] = a.m[row] * b.m[column * 4] + a.m[4 + row] * b.m[column * 4 + 1] +
                                      a.m[8 + row] * b.m[column * 4 + 2] + a.m[12 + row] * b.m[column * 4 + 3];
        }
    }
    return out;
}

Vertex decode_vertex(const VertexLayout &layout, const std::uint8_t *data, const float *morph_weights) {
    Vertex vertex;
    std::fill(vertex.color.begin(), vertex.color.end(), 0.0f);
    const bool through = layout.through;
    const int morphs = through ? 1 : layout.morph_count;
    const std::size_t target_size =
        layout.morph_count != 0u ? layout.stride / layout.morph_count : static_cast<std::size_t>(layout.stride);

    for (int morph = 0; morph < morphs; ++morph) {
        const std::uint8_t *target = data + static_cast<std::size_t>(morph) * target_size;
        const float weight = morphs == 1 ? 1.0f : (morph_weights != nullptr ? morph_weights[morph] : (morph == 0 ? 1.0f : 0.0f));
        if (layout.weight != 0u) {
            for (int i = 0; i < layout.weight_count; ++i) {
                const float scale = layout.weight == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
                vertex.weights[i] += weight * component(target, layout.weight, i, false, scale);
            }
        }
        if (layout.texcoord != 0u) {
            const float scale = through ? 1.0f : layout.texcoord == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (int i = 0; i < 2; ++i)
                vertex.texcoord[i] += weight * component(target + layout.texcoord_offset, layout.texcoord, i, false, scale);
        }
        if (layout.color >= 4u) {
            std::array<float, 4> color{};
            decode_color(target + layout.color_offset, layout.color, color);
            for (int i = 0; i < 4; ++i) vertex.color[i] += weight * color[i];
        }
        if (layout.normal != 0u) {
            const float scale = layout.normal == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (int i = 0; i < 3; ++i)
                vertex.normal[i] += weight * component(target + layout.normal_offset, layout.normal, i, true, scale);
        }
        const int position_kind = layout.position != 0u ? layout.position : 3u;
        if (through) {
            for (int i = 0; i < 3; ++i)
                vertex.position[i] = component(target + layout.position_offset, position_kind, i, i < 2, 1.0f);
        } else {
            const float scale = position_kind == 1u ? 1.0f / 128.0f : 1.0f / 32768.0f;
            for (int i = 0; i < 3; ++i)
                vertex.position[i] +=
                    weight * component(target + layout.position_offset, position_kind, i, true, scale);
        }
    }
    if (layout.color < 4u) vertex.color[0] = -1.0f;
    return vertex;
}

void decode_vertices(const VertexLayout &layout, const std::uint8_t *data, std::uint32_t count,
                     const float *morph_weights, std::vector<Vertex> &out) {
    out.resize(count);
    for (std::uint32_t i = 0; i < count; ++i)
        out[i] = decode_vertex(layout, data + static_cast<std::size_t>(i) * layout.stride, morph_weights);
}

VertexPipeline::VertexPipeline(const TransformState &state) {
    set_state(state);
}

void VertexPipeline::set_state(const TransformState &state) {
    state_ = state;
    configure();
}

void VertexPipeline::configure() {
    view_world_ = state_.view * state_.world;
    world_view_projection_ = state_.projection * view_world_;
    if (state_.widescreen > 1.0f && !state_.through) {
        // Widen the frustum: the horizontal row shrinks by the same factor the
        // projection step scales the other way, so the picture keeps its
        // pixel size and the extra columns show world the native frustum cut.
        const float inverse = 1.0f / state_.widescreen;
        // Column-major storage: the x row is indices 0, 4, 8, 12.
        world_view_projection_.m[0] *= inverse;
        world_view_projection_.m[4] *= inverse;
        world_view_projection_.m[8] *= inverse;
        world_view_projection_.m[12] *= inverse;
    }
    need_world_ = state_.lighting.enabled;
    need_normal_ = state_.lighting.enabled ||
                   (state_.texture_map.enabled && state_.texture_map.mode == TextureMode::Matrix &&
                    state_.texture_map.source == TextureSource::Normal);
}

void VertexPipeline::shade(const Vertex &vertex, Vec3 world_position, Vec3 world_normal, ClipVertex &out) const {
    const LightingState &lighting = state_.lighting;
    const Material &material = lighting.material;
    float alpha = material.alpha;
    const Vec3 material_ambient = material_color(vertex, material, 1u, alpha);
    const Vec3 material_diffuse = material_color(vertex, material, 2u, alpha);
    const Vec3 material_specular = material_color(vertex, material, 4u, alpha);

    const Vec3 normal = normalize(lighting.reverse_normal ? world_normal * -1.0f : world_normal);
    Vec3 sum = material.emissive + multiply(lighting.ambient, material_ambient);
    Vec3 specular_sum;

    for (const Light &light : lighting.lights) {
        if (!light.enabled) continue;
        Vec3 to_light;
        float attenuation = 1.0f;
        if (light.type == LightType::Directional) {
            to_light = normalize(light.position);
        } else {
            const Vec3 delta = light.position - world_position;
            const float distance = std::sqrt(dot(delta, delta));
            to_light = distance > 0.0f ? delta * (1.0f / distance) : delta;
            const float denominator = light.attenuation.x + light.attenuation.y * distance +
                                      light.attenuation.z * distance * distance;
            attenuation = denominator > 0.0f ? std::min(1.0f / denominator, 1.0f) : 1.0f;
            if (light.type == LightType::Spot) {
                const float spot = dot(to_light * -1.0f, normalize(light.direction));
                attenuation = spot >= light.cutoff ? attenuation * std::pow(std::max(spot, 0.0f), light.exponent) : 0.0f;
            }
        }

        float ndotl = dot(normal, to_light);
        if (light.kind == LightKind::PoweredDiffuse)
            ndotl = ndotl > 0.0f ? std::pow(ndotl, material.specular_coefficient) : 0.0f;
        if (ndotl > 0.0f) {
            sum = sum + multiply(light.diffuse, material_diffuse) * (attenuation * ndotl);
            if (light.kind == LightKind::DiffuseSpecular) {
                const Vec3 half_vector = normalize(to_light + Vec3{0.0f, 0.0f, 1.0f});
                const float ndoth = std::max(dot(normal, half_vector), 0.0f);
                specular_sum = specular_sum + multiply(light.specular, material_specular) *
                                                  (attenuation * std::pow(ndoth, material.specular_coefficient));
            }
        }
        sum = sum + multiply(light.ambient, material_ambient) * attenuation;
    }

    if (!lighting.separate_specular) {
        sum = sum + specular_sum;
        specular_sum = {};
    }
    out.color = {std::clamp(sum.x, 0.0f, 1.0f), std::clamp(sum.y, 0.0f, 1.0f), std::clamp(sum.z, 0.0f, 1.0f),
                 std::clamp(lighting.ambient_alpha * alpha, 0.0f, 1.0f)};
    out.specular = {std::clamp(specular_sum.x, 0.0f, 1.0f), std::clamp(specular_sum.y, 0.0f, 1.0f),
                    std::clamp(specular_sum.z, 0.0f, 1.0f)};
}

ClipVertex VertexPipeline::transform(const Vertex &vertex) const {
    ClipVertex out;
    const auto material_fallback = [this, &out]() {
        out.color = {state_.lighting.material.ambient.x, state_.lighting.material.ambient.y,
                     state_.lighting.material.ambient.z, state_.lighting.material.alpha};
    };

    if (state_.through) {
        out.clip = {vertex.position[0], vertex.position[1], vertex.position[2], 1.0f};
        out.texcoord = {vertex.texcoord[0], vertex.texcoord[1], 1.0f};
        if (state_.texture_map.enabled) {
            out.texcoord[0] /= state_.texture_map.width;
            out.texcoord[1] /= state_.texture_map.height;
        }
        if (vertex.color[0] >= 0.0f)
            out.color = vertex.color;
        else
            material_fallback();
        out.fog = 1.0f;
        return out;
    }

    Vec3 position{vertex.position[0], vertex.position[1], vertex.position[2]};
    Vec3 normal{vertex.normal[0], vertex.normal[1], vertex.normal[2]};
    if (state_.layout.weight != 0u) {
        Vec3 skinned_position;
        Vec3 skinned_normal;
        for (int i = 0; i < state_.layout.weight_count; ++i) {
            const float weight = vertex.weights[i];
            if (weight == 0.0f) continue;
            skinned_position = skinned_position + state_.bones[i].transform(position) * weight;
            if (need_normal_) skinned_normal = skinned_normal + state_.bones[i].transform(normal, 0.0f) * weight;
        }
        position = skinned_position;
        if (need_normal_) normal = skinned_normal;
    }

    const Vec4 clip = world_view_projection_.transform({position.x, position.y, position.z, 1.0f});
    out.clip = {clip.x, clip.y, clip.z, clip.w};

    Vec3 world_position;
    Vec3 world_normal;
    if (need_world_) world_position = state_.world.transform(position);
    if (need_normal_) world_normal = state_.world.transform(normal, 0.0f);

    if (state_.lighting.enabled)
        shade(vertex, world_position, world_normal, out);
    else if (vertex.color[0] >= 0.0f)
        out.color = vertex.color;
    else
        material_fallback();

    if (state_.texture_map.enabled && state_.texture_map.mode == TextureMode::Matrix) {
        Vec3 source;
        switch (state_.texture_map.source) {
        case TextureSource::Position:
            source = position;
            break;
        case TextureSource::Uv:
            source = {vertex.texcoord[0], vertex.texcoord[1], 0.0f};
            break;
        case TextureSource::Normal:
            source = normalize(normal);
            break;
        }
        const Vec3 transformed = state_.texture.transform(source);
        out.texcoord = {transformed.x, transformed.y, transformed.z};
    } else if (state_.texture_map.enabled) {
        out.texcoord = {vertex.texcoord[0] * state_.texture_map.scale[0] + state_.texture_map.offset[0],
                        vertex.texcoord[1] * state_.texture_map.scale[1] + state_.texture_map.offset[1], 1.0f};
    } else {
        out.texcoord = {vertex.texcoord[0], vertex.texcoord[1], 1.0f};
    }
    // Fog from view-space depth: 1 = no fog, 0 = full fog (PPSSPP
    // convention). Through-mode and disabled fog both yield 1.
    if (state_.fog_enabled) {
        const float view_z = view_world_.transform(position).z;
        const float end = state_.fog_end;
        const float slope = state_.fog_slope;
        float factor = 1.0f;
        if (std::isinf(end) || std::isnan(end)) {
            // Games leave fog enabled with inf/nan end to force a constant
            // factor (PPSSPP TransformUnit: required for Outrun skies).
            bool full = std::signbit(end) ? true : false;
            if (std::signbit(slope)) full = !full;
            if (slope == 0.0f) full = true;
            factor = full ? 0.0f : 1.0f;
        } else {
            float use_slope = slope;
            if (std::isinf(use_slope) || std::isnan(use_slope))
                use_slope = std::signbit(use_slope) ? -262144.0f : 262144.0f;
            factor = (view_z + end) * use_slope;
        }
        out.fog = std::clamp(factor, 0.0f, 1.0f);
    } else {
        out.fog = 1.0f;
    }
    return out;
}

ScreenVertex VertexPipeline::project(const ClipVertex &vertex) const {
    ScreenVertex out;
    out.texcoord = vertex.texcoord;
    out.color = vertex.color;
    out.specular = vertex.specular;
    out.fog = vertex.fog;
    if (state_.through) {
        // Screen-space: never stretched, only recentred in the wider frame.
        const float margin = state_.widescreen > 1.0f ? 240.0f * (state_.widescreen - 1.0f) : 0.0f;
        out.x = vertex.clip[0] + margin;
        out.y = vertex.clip[1];
        out.z = vertex.clip[2];
        out.inv_w = 1.0f;
        return out;
    }

    const float w = vertex.clip[3];
    const float inv_w = w != 0.0f ? 1.0f / w : 0.0f;
    out.inv_w = inv_w;
    const float widen = state_.widescreen > 1.0f ? state_.widescreen : 1.0f;
    out.x = (vertex.clip[0] * inv_w * state_.viewport.scale[0] +
             state_.viewport.center[0] - state_.viewport.offset_x) * widen;
    out.y = vertex.clip[1] * inv_w * state_.viewport.scale[1] + state_.viewport.center[1] - state_.viewport.offset_y;
    out.z = std::clamp(vertex.clip[2] * inv_w * state_.viewport.scale[2] + state_.viewport.center[2], 0.0f, 65535.0f);
    return out;
}

float signed_area(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool VertexPipeline::cull_triangle(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c,
                                   CullMode mode, FrontFace front_face) {
    if (mode == CullMode::None) return false;
    const float area = signed_area(a, b, c);
    // Screen space is y-down: positive area is counter-clockwise. FFACE
    // (0x9B bit 0) selects which winding is front; both reference impls
    // cull `cullCCW ? area < 0 : area > 0`.
    const bool front = front_face == FrontFace::Clockwise ? area < 0.0f : area > 0.0f;
    return mode == CullMode::Back ? !front : front;
}

std::vector<ClipVertex> clip_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c) {
    std::vector<ClipVertex> polygon{a, b, c};
    polygon = clip_plane(polygon, plane_distance_near);
    polygon = clip_plane(polygon, plane_distance_far);
    return polygon;
}

void clip_triangle_into(std::vector<ClipVertex> &out, std::vector<ClipVertex> &scratch, const ClipVertex &a,
                        const ClipVertex &b, const ClipVertex &c) {
    out.clear();
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    clip_plane_into(scratch, out, plane_distance_near);
    clip_plane_into(out, scratch, plane_distance_far);
}

} // namespace gungnir::ge

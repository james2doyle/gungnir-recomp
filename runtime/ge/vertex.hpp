#pragma once

// GE vertex pipeline (T7b): format decoding, skinning, world/view/projection,
// texture matrices, lighting and clip-space output. The matrix conventions
// follow the GE: 4x3 matrices are column-major with an implicit fourth row
// (1), 4x4 matrices are column-major. This module is standalone and has no
// renderer or Vulkan dependency.

#include "ge/display_list.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gungnir::ge {

struct Vec3 {
    float x{};
    float y{};
    float z{};

    bool operator==(const Vec3 &) const = default;
};

struct Vec4 {
    float x{};
    float y{};
    float z{};
    float w{};

    bool operator==(const Vec4 &) const = default;
};

Vec3 operator+(Vec3 a, Vec3 b);
Vec3 operator-(Vec3 a, Vec3 b);
Vec3 operator*(Vec3 a, float scalar);
Vec3 multiply(Vec3 a, Vec3 b);
float dot(Vec3 a, Vec3 b);
Vec3 normalize(Vec3 value);

struct Mat43 {
    std::array<float, 12> m{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};

    static Mat43 identity() { return Mat43{}; }
    Vec3 transform(Vec3 value, float w = 1.0f) const;

    bool operator==(const Mat43 &) const = default;
};

struct Mat44 {
    std::array<float, 16> m{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    static Mat44 identity() { return Mat44{}; }
    Vec4 transform(Vec4 value) const;

    bool operator==(const Mat44 &) const = default;
};

// a * b, with the implicit fourth row of the 4x3 operand.
Mat43 operator*(const Mat43 &a, const Mat43 &b);
Mat44 operator*(const Mat44 &a, const Mat43 &b);
Mat44 operator*(const Mat44 &a, const Mat44 &b);

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 2> texcoord{};
    std::array<float, 4> color{-1.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 3> normal{};
    std::array<float, 8> weights{};

    bool operator==(const Vertex &) const = default;
};

Vertex decode_vertex(const VertexLayout &layout, const std::uint8_t *data, const float *morph_weights);
void decode_vertices(const VertexLayout &layout, const std::uint8_t *data, std::uint32_t count,
                     const float *morph_weights, std::vector<Vertex> &out);

struct ClipVertex {
    std::array<float, 4> clip{};
    std::array<float, 3> texcoord{0.0f, 0.0f, 1.0f}; // s, t, q
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> specular{};
    float fog{1.0f};

    bool operator==(const ClipVertex &) const = default;
};

struct ScreenVertex {
    float x{};
    float y{};
    float z{};
    float inv_w{1.0f};
    std::array<float, 3> texcoord{0.0f, 0.0f, 1.0f};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 3> specular{};
    float fog{1.0f};

    bool operator==(const ScreenVertex &) const = default;
};

struct Viewport {
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<float, 3> center{};
    float offset_x{};
    float offset_y{};
};

enum class TextureMode : std::uint8_t { Uv, Matrix };
enum class TextureSource : std::uint8_t { Position, Uv, Normal };

struct TextureMap {
    bool enabled{};
    float width{1.0f};
    float height{1.0f};
    std::array<float, 2> scale{1.0f, 1.0f};
    std::array<float, 2> offset{};
    TextureMode mode{TextureMode::Uv};
    TextureSource source{TextureSource::Position};
};

enum class LightType : std::uint8_t { Directional, Point, Spot };
enum class LightKind : std::uint8_t { Diffuse, DiffuseSpecular, PoweredDiffuse };

struct Light {
    bool enabled{};
    LightType type{LightType::Directional};
    LightKind kind{LightKind::Diffuse};
    Vec3 position{};
    Vec3 direction{};
    Vec3 attenuation{1.0f, 0.0f, 0.0f};
    Vec3 ambient{};
    Vec3 diffuse{};
    Vec3 specular{};
    float exponent{};
    float cutoff{};

    bool operator==(const Light &) const = default;
};

struct Material {
    Vec3 ambient{1.0f, 1.0f, 1.0f};
    Vec3 diffuse{1.0f, 1.0f, 1.0f};
    Vec3 specular{1.0f, 1.0f, 1.0f};
    Vec3 emissive{};
    float alpha{1.0f};
    float specular_coefficient{};
    std::uint8_t update{};

    bool operator==(const Material &) const = default;
};

struct LightingState {
    bool enabled{};
    bool separate_specular{};
    bool reverse_normal{};
    Material material{};
    Vec3 ambient{};
    float ambient_alpha{1.0f};
    std::array<Light, 4> lights{};

    bool operator==(const LightingState &) const = default;
};

enum class CullMode : std::uint8_t { None, Front, Back };
enum class FrontFace : std::uint8_t { Clockwise, CounterClockwise };

struct TransformState {
    Mat43 world{Mat43::identity()};
    Mat43 view{Mat43::identity()};
    Mat43 texture{Mat43::identity()};
    Mat44 projection{Mat44::identity()};
    Viewport viewport{};
    TextureMap texture_map{};
    LightingState lighting{};
    CullMode cull{CullMode::None};
    FrontFace front_face{FrontFace::Clockwise};
    std::array<Mat43, 8> bones{};
    // Fog (0x1F/0xCD/0xCE): view-space depth factor, 1 = no fog.
    bool fog_enabled{};
    float fog_end{};
    float fog_slope{1.0f};
    VertexLayout layout{};
    bool through{};
    // Host widescreen widening (T16a): 1 is the native 480x272 mapping. Above
    // 1 the perspective frustum narrows horizontally by the same factor and
    // the mapping grows, so the scene keeps its pixel size and the frame shows
    // more world; screen-space (through) draws are only recentred. Guest state:
    // no.
    float widescreen{1.0f};

    bool operator==(const TransformState &) const = default;
};

class VertexPipeline {
public:
    VertexPipeline() = default;
    explicit VertexPipeline(const TransformState &state);

    void set_state(const TransformState &state);
    [[nodiscard]] const TransformState &state() const { return state_; }

    [[nodiscard]] ClipVertex transform(const Vertex &vertex) const;
    [[nodiscard]] ScreenVertex project(const ClipVertex &vertex) const;
    static bool cull_triangle(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c,
                              CullMode mode, FrontFace front_face);

private:
    void configure();
    void shade(const Vertex &vertex, Vec3 world_position, Vec3 world_normal, ClipVertex &out) const;

    TransformState state_;
    Mat43 view_world_{Mat43::identity()};
    Mat44 world_view_projection_{Mat44::identity()};
    bool need_world_{};
    bool need_normal_{};
};

float signed_area(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c);

// Clips a triangle against the GE near (z >= -w) and far (z <= w) planes.
// The result is a convex polygon with 0..5 vertices.
std::vector<ClipVertex> clip_triangle(const ClipVertex &a, const ClipVertex &b, const ClipVertex &c);
void clip_triangle_into(std::vector<ClipVertex> &out, std::vector<ClipVertex> &scratch, const ClipVertex &a,
                        const ClipVertex &b, const ClipVertex &c);

} // namespace gungnir::ge

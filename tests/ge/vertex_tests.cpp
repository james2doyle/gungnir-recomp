// T7b contract tests: vertex formats, transforms, clipping and culling.
//
// Inputs and expected values are hand-computed; no game data is used.

#include "ge/display_list.hpp"
#include "ge/vertex.hpp"

#include "psprecomp/allegrex_context.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace gungnir::ge;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

bool close(float a, float b, float tolerance = 1.0e-5f) {
    return std::fabs(a - b) <= tolerance;
}

void store_u16(std::uint8_t *data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value & 0xFFu);
    data[1] = static_cast<std::uint8_t>(value >> 8u);
}

void store_u32(std::uint8_t *data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value & 0xFFu);
    data[1] = static_cast<std::uint8_t>((value >> 8u) & 0xFFu);
    data[2] = static_cast<std::uint8_t>((value >> 16u) & 0xFFu);
    data[3] = static_cast<std::uint8_t>((value >> 24u) & 0xFFu);
}

void store_float(std::uint8_t *data, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    store_u32(data, bits);
}

void vertex_formats() {
    const VertexLayout mixed = decode_vertex_layout(0x000491BEu);
    std::vector<std::uint8_t> data(mixed.stride * 2u, 0u);
    store_u16(data.data(), 0x4000u);
    store_u16(data.data() + 2u, 0x8000u);
    store_u32(data.data() + 4u, 0xFF804020u);
    data[8] = 0x40u;
    data[9] = 0xC0u;
    data[10] = 0x20u;
    store_float(data.data() + 12u, 1.5f);
    store_float(data.data() + 16u, -2.5f);
    store_float(data.data() + 20u, 3.0f);

    store_float(data.data() + 24u + 12u, 2.5f);
    store_float(data.data() + 24u + 16u, -3.5f);
    store_float(data.data() + 24u + 20u, 4.0f);

    const float first_weights[2] = {1.0f, 0.0f};
    const Vertex first = decode_vertex(mixed, data.data(), first_weights);
    check(close(first.position[0], 1.5f) && close(first.position[1], -2.5f) && close(first.position[2], 3.0f),
          "float position decoded");
    check(close(first.texcoord[0], 0.5f) && close(first.texcoord[1], 1.0f), "16-bit texcoord scaled by 1/32768");
    check(close(first.color[0], 32.0f / 255.0f) && close(first.color[1], 64.0f / 255.0f) &&
              close(first.color[2], 128.0f / 255.0f) && close(first.color[3], 1.0f),
          "8888 color decoded");
    check(close(first.normal[0], 0.5f) && close(first.normal[1], -0.5f) && close(first.normal[2], 0.25f),
          "8-bit normal scaled by 1/128");

    const float half_weights[2] = {0.5f, 0.5f};
    const Vertex blended = decode_vertex(mixed, data.data(), half_weights);
    check(close(blended.position[0], 2.0f) && close(blended.position[1], -3.0f) && close(blended.position[2], 3.5f),
          "morph targets blend by weight");

    const std::uint32_t position_only[] = {(1u << 7u), (2u << 7u), (3u << 7u)};
    constexpr float position_8 = 0.25f, position_32 = 1.25f;
    std::array<std::uint8_t, 12> bytes{};
    bytes[0] = 0x20u;
    bytes[1] = 0xC0u;
    bytes[2] = 0x40u;
    VertexLayout layout = decode_vertex_layout(position_only[0]);
    Vertex decoded = decode_vertex(layout, bytes.data(), nullptr);
    check(close(decoded.position[0], position_8) && close(decoded.position[1], -0.5f) && close(decoded.position[2], 0.5f),
          "8-bit position scaled by 1/128");

    layout = decode_vertex_layout(position_only[1]);
    store_u16(bytes.data(), 0x4000u);
    store_u16(bytes.data() + 2u, 0x8000u);
    store_u16(bytes.data() + 4u, 0x2000u);
    decoded = decode_vertex(layout, bytes.data(), nullptr);
    check(close(decoded.position[0], 0.5f) && close(decoded.position[1], -1.0f) && close(decoded.position[2], 0.25f),
          "16-bit position scaled by 1/32768");

    layout = decode_vertex_layout(position_only[2]);
    store_float(bytes.data(), position_32);
    store_float(bytes.data() + 4u, -2.0f);
    store_float(bytes.data() + 8u, 0.5f);
    decoded = decode_vertex(layout, bytes.data(), nullptr);
    check(close(decoded.position[0], 1.25f) && close(decoded.position[1], -2.0f) && close(decoded.position[2], 0.5f),
          "float position decoded");

    const std::uint32_t color_bits[] = {4u << 2u, 5u << 2u, 6u << 2u, 7u << 2u};
    for (const std::uint32_t color : color_bits) {
        const VertexLayout color_layout = decode_vertex_layout(color | (3u << 7u));
        std::array<std::uint8_t, 16> color_data{};
        if (color == (7u << 2u)) {
            store_u32(color_data.data(), 0xFFFFFFFFu);
        } else {
            store_u16(color_data.data(), 0xFFFFu);
        }
        const Vertex color_vertex = decode_vertex(color_layout, color_data.data(), nullptr);
        check(close(color_vertex.color[0], 1.0f) && close(color_vertex.color[1], 1.0f) &&
                  close(color_vertex.color[2], 1.0f) && close(color_vertex.color[3], 1.0f),
              "every color format decodes white");
    }

    const VertexLayout through_layout = decode_vertex_layout((2u << 0u) | (7u << 2u) | (2u << 7u) | (1u << 23u));
    std::array<std::uint8_t, 16> through_data{};
    store_u16(through_data.data(), 64u);
    store_u16(through_data.data() + 2u, 32u);
    store_u32(through_data.data() + 4u, 0xFFFFFFFFu);
    store_u16(through_data.data() + 8u, 100u);
    store_u16(through_data.data() + 10u, 50u);
    store_u16(through_data.data() + 12u, 25u);
    const Vertex through = decode_vertex(through_layout, through_data.data(), nullptr);
    check(through.position[0] == 100.0f && through.position[1] == 50.0f && through.position[2] == 25.0f,
          "through-mode position is screen space");
    check(through.texcoord[0] == 64.0f && through.texcoord[1] == 32.0f, "through-mode texcoord is texels");

    const VertexLayout weight_layout = decode_vertex_layout((1u << 9u) | (3u << 14u) | (3u << 7u));
    std::array<std::uint8_t, 16> weight_data{};
    weight_data[0] = 0x40u;
    weight_data[1] = 0x20u;
    weight_data[2] = 0x10u;
    store_float(weight_data.data() + 4u, 1.0f);
    store_float(weight_data.data() + 8u, 2.0f);
    store_float(weight_data.data() + 12u, 3.0f);
    const Vertex weighted = decode_vertex(weight_layout, weight_data.data(), nullptr);
    check(close(weighted.weights[0], 0.5f) && close(weighted.weights[1], 0.25f) && close(weighted.weights[2], 0.125f),
          "8-bit weights scaled by 1/128");
}

TransformState base_state() {
    TransformState state;
    state.layout = decode_vertex_layout(3u << 7u);
    state.viewport.scale = {240.0f, 136.0f, 1.0f};
    state.viewport.center = {240.0f, 136.0f, 0.0f};
    return state;
}

Vertex position_vertex(float x, float y, float z) {
    Vertex vertex;
    vertex.position = {x, y, z};
    vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
    return vertex;
}

void transform_and_viewport() {
    const TransformState state = base_state();
    const VertexPipeline pipeline(state);
    const ClipVertex clip = pipeline.transform(position_vertex(-1.0f, -1.0f, 0.5f));
    check(clip.clip[0] == -1.0f && clip.clip[1] == -1.0f && clip.clip[2] == 0.5f && clip.clip[3] == 1.0f,
          "identity transform leaves clip coordinates");
    const ScreenVertex screen = pipeline.project(clip);
    check(screen.x == 0.0f && screen.y == 0.0f && close(screen.z, 0.5f), "viewport maps the lower-left corner");
    check(close(screen.inv_w, 1.0f), "orthographic inverse w is one");
    check(screen.color[0] == 1.0f && screen.color[3] == 1.0f, "vertex color passes through unlit");

    const ScreenVertex upper = pipeline.project(pipeline.transform(position_vertex(1.0f, 1.0f, 0.5f)));
    check(upper.x == 480.0f && upper.y == 272.0f, "viewport maps the upper-right corner");
}

void composition_and_scalar_parity() {
    TransformState state = base_state();
    state.world.m[9] = 10.0f;
    state.world.m[10] = 20.0f;
    state.world.m[11] = 30.0f;
    state.view.m[9] = 1.0f;
    state.view.m[10] = 2.0f;
    state.view.m[11] = 3.0f;
    state.viewport.scale = {1.0f, 1.0f, 1.0f};
    state.viewport.center = {0.0f, 0.0f, 0.0f};
    const VertexPipeline pipeline(state);
    const ClipVertex composed = pipeline.transform(position_vertex(1.0f, 2.0f, 3.0f));
    check(composed.clip[0] == 12.0f && composed.clip[1] == 24.0f && composed.clip[2] == 36.0f,
          "view * world adds both translations");

    const Vec3 world = state.world.transform({1.0f, 2.0f, 3.0f});
    const Vec3 viewed = state.view.transform(world);
    const Vec4 sequential = state.projection.transform({viewed.x, viewed.y, viewed.z, 1.0f});
    check(composed.clip[0] == sequential.x && composed.clip[1] == sequential.y && composed.clip[2] == sequential.z,
          "composed transform equals sequential scalar transforms");
    check(composed.clip[3] == sequential.w, "composed w equals sequential w");

    TransformState scaled = state;
    scaled.projection.m[0] = 2.0f;
    scaled.projection.m[5] = 3.0f;
    scaled.projection.m[10] = 4.0f;
    const ClipVertex projected = VertexPipeline(scaled).transform(position_vertex(1.0f, 2.0f, 3.0f));
    check(projected.clip[0] == 24.0f && projected.clip[1] == 72.0f && projected.clip[2] == 144.0f,
          "projection composes after view and world");
}

ClipVertex clip_vertex(float z, float w, float red = 1.0f) {
    ClipVertex vertex;
    vertex.clip = {0.0f, 0.0f, z, w};
    vertex.texcoord = {0.0f, 0.0f, 1.0f};
    vertex.color = {red, 0.0f, 0.0f, 1.0f};
    return vertex;
}

void clipping() {
    const std::vector<ClipVertex> all_inside = clip_triangle(clip_vertex(0.0f, 1.0f), clip_vertex(0.5f, 1.0f),
                                                             clip_vertex(1.0f, 1.0f));
    check(all_inside.size() == 3u, "fully inside triangle is unchanged");

    const std::vector<ClipVertex> near = clip_triangle(clip_vertex(-2.0f, 1.0f), clip_vertex(0.0f, 1.0f),
                                                       clip_vertex(1.0f, 1.0f));
    check(near.size() == 4u, "near-clipped triangle becomes a quad");
    bool found_near = false;
    for (const ClipVertex &vertex : near)
        if (close(vertex.clip[2], -1.0f) && close(vertex.clip[3], 1.0f)) found_near = true;
    check(found_near, "near clip interpolates to z = -w");

    const std::vector<ClipVertex> outside_near = clip_triangle(clip_vertex(-3.0f, 1.0f), clip_vertex(-2.0f, 1.0f),
                                                               clip_vertex(-1.5f, 1.0f));
    check(outside_near.empty(), "triangle behind the near plane is discarded");

    const std::vector<ClipVertex> far = clip_triangle(clip_vertex(2.0f, 1.0f), clip_vertex(0.0f, 1.0f),
                                                      clip_vertex(0.5f, 1.0f));
    check(far.size() == 4u, "far-clipped triangle becomes a quad");
    bool found_far = false;
    for (const ClipVertex &vertex : far)
        if (close(vertex.clip[2], 1.0f) && close(vertex.clip[3], 1.0f)) found_far = true;
    check(found_far, "far clip interpolates to z = w");
}

void culling() {
    // Screen space is y-down: a positive signed area is counter-clockwise,
    // which is front when FFACE (0x9B) selects counter-clockwise; a
    // negative area is front under clockwise. Both reference impls cull
    // `cullCCW ? area < 0 : area > 0`.
    ScreenVertex a{0.0f, 0.0f};
    ScreenVertex b{10.0f, 0.0f};
    ScreenVertex c{0.0f, 10.0f};
    check(signed_area(a, b, c) > 0.0f, "screen triangle has positive area");
    check(!VertexPipeline::cull_triangle(a, b, c, CullMode::None, FrontFace::CounterClockwise),
          "no culling keeps faces");
    check(!VertexPipeline::cull_triangle(a, b, c, CullMode::Back, FrontFace::CounterClockwise),
          "back cull keeps front");
    check(VertexPipeline::cull_triangle(a, b, c, CullMode::Front, FrontFace::CounterClockwise),
          "front cull drops front");
    check(VertexPipeline::cull_triangle(a, b, c, CullMode::Back, FrontFace::Clockwise),
          "positive area is back under clockwise winding");

    ScreenVertex reverse{0.0f, 10.0f};
    check(VertexPipeline::cull_triangle(a, reverse, b, CullMode::Back, FrontFace::CounterClockwise),
          "negative area is back under counter-clockwise winding");
    check(!VertexPipeline::cull_triangle(a, reverse, b, CullMode::Front, FrontFace::CounterClockwise),
          "front cull keeps back faces");

    ScreenVertex degenerate{0.0f, 0.0f};
    check(VertexPipeline::cull_triangle(a, degenerate, degenerate, CullMode::Back, FrontFace::Clockwise),
          "degenerate triangles are culled");
    check(!VertexPipeline::cull_triangle(a, degenerate, degenerate, CullMode::None, FrontFace::Clockwise),
          "no culling keeps degenerate triangles");
}

void lighting_and_material() {
    TransformState state = base_state();
    state.lighting.enabled = true;
    state.lighting.material.ambient = {0.5f, 0.5f, 0.5f};
    state.lighting.material.diffuse = {0.5f, 0.5f, 0.5f};
    state.lighting.material.specular = {0.0f, 0.0f, 0.0f};
    state.lighting.ambient = {0.0f, 0.0f, 0.0f};
    Light &light = state.lighting.lights[0];
    light.enabled = true;
    light.type = LightType::Directional;
    light.kind = LightKind::Diffuse;
    light.position = {0.0f, 0.0f, 1.0f};
    light.ambient = {0.2f, 0.2f, 0.2f};
    light.diffuse = {0.8f, 0.8f, 0.8f};

    Vertex vertex;
    vertex.position = {0.0f, 0.0f, 0.0f};
    vertex.normal = {0.0f, 0.0f, 1.0f};
    vertex.color[0] = -1.0f;
    const VertexPipeline pipeline(state);
    const ClipVertex lit = pipeline.transform(vertex);
    check(close(lit.color[0], 0.5f) && close(lit.color[1], 0.5f) && close(lit.color[2], 0.5f),
          "ambient plus diffuse light produces the hand-computed color");
    check(close(lit.color[3], 1.0f), "material alpha reaches the output");
    check(close(lit.specular[0], 0.0f), "diffuse-only light has no specular");

    TransformState separate = state;
    separate.lighting.separate_specular = true;
    separate.lighting.material.specular = {1.0f, 1.0f, 1.0f};
    separate.lighting.material.specular_coefficient = 1.0f;
    separate.lighting.lights[0].kind = LightKind::DiffuseSpecular;
    separate.lighting.lights[0].specular = {1.0f, 1.0f, 1.0f};
    const VertexPipeline separate_pipeline(separate);
    const ClipVertex with_specular = separate_pipeline.transform(vertex);
    check(close(with_specular.color[0], 0.5f), "separate specular stays out of the base color");
    check(close(with_specular.specular[0], 1.0f) && close(with_specular.specular[1], 1.0f),
          "separate specular reaches the specular attribute");
}

void through_and_texture_matrix() {
    TransformState state;
    state.through = true;
    state.layout = decode_vertex_layout((2u << 0u) | (7u << 2u) | (2u << 7u) | (1u << 23u));
    state.texture_map.enabled = true;
    state.texture_map.width = 256.0f;
    state.texture_map.height = 128.0f;
    Vertex vertex;
    vertex.position = {100.0f, 50.0f, 25.0f};
    vertex.texcoord = {64.0f, 32.0f};
    vertex.color = {1.0f, 1.0f, 1.0f, 1.0f};
    const VertexPipeline through(state);
    const ClipVertex clip = through.transform(vertex);
    check(clip.clip[0] == 100.0f && clip.clip[1] == 50.0f && clip.clip[2] == 25.0f && clip.clip[3] == 1.0f,
          "through mode bypasses the transform");
    check(close(clip.texcoord[0], 0.25f) && close(clip.texcoord[1], 0.25f), "through texcoords divide by texture size");
    const ScreenVertex screen = through.project(clip);
    check(screen.x == 100.0f && screen.y == 50.0f && screen.z == 25.0f, "through projection is direct");

    TransformState matrix = base_state();
    matrix.texture_map.enabled = true;
    matrix.texture_map.mode = TextureMode::Matrix;
    matrix.texture_map.source = TextureSource::Uv;
    matrix.texture.m[9] = 0.5f;
    matrix.texture.m[10] = 0.25f;
    matrix.texture.m[11] = 1.0f;
    Vertex uv;
    uv.position = {0.0f, 0.0f, 0.0f};
    uv.texcoord = {0.2f, 0.4f};
    uv.color = {1.0f, 1.0f, 1.0f, 1.0f};
    const ClipVertex projected = VertexPipeline(matrix).transform(uv);
    check(close(projected.texcoord[0], 0.7f) && close(projected.texcoord[1], 0.65f) &&
              close(projected.texcoord[2], 1.0f),
          "texture matrix transforms the selected source");
}

// T16a: the scene is widened, the HUD is not. With scale k the frustum's
// horizontal half-extent shrinks by k and the mapping grows by k, so the
// picture keeps its pixel size and the extra columns show world that the
// native frustum cut off; screen-space (through) draws are only recentred.
void widescreen_widening() {
    constexpr float k = 1.3f;
    TransformState native = base_state();
    native.viewport.scale = {240.0f, 136.0f, 1.0f};
    native.viewport.center = {240.0f, 136.0f, 0.0f};
    TransformState wide = native;
    wide.widescreen = k;

    // An off-centre point at the edge of the native frustum.
    const ClipVertex native_clip = VertexPipeline(native).transform(position_vertex(0.5f, 0.0f, 0.5f));
    const ScreenVertex native_screen = VertexPipeline(native).project(native_clip);
    const ScreenVertex wide_screen = VertexPipeline(wide).project(VertexPipeline(wide).transform(position_vertex(0.5f, 0.0f, 0.5f)));
    check(close(wide_screen.x, native_screen.x + 240.0f * (k - 1.0f), 1.0e-3f),
          "a widened scene keeps the native pixel size and shifts only by the new margin");
    check(close(wide_screen.y, native_screen.y), "widening never touches the vertical mapping");

    // A point the native frustum clipped now lands inside the widened frame.
    const ScreenVertex outside = VertexPipeline(wide).project(VertexPipeline(wide).transform(position_vertex(1.0f, 0.0f, 0.5f)));
    check(outside.x > 480.0f && outside.x <= 480.0f * k,
          "geometry beyond the native frustum lands inside the widened frame");

    // Through (HUD) draws are screen-space: no scale, recentred by the margin.
    TransformState through_native;
    through_native.through = true;
    TransformState through_wide = through_native;
    through_wide.widescreen = k;
    Vertex hud;
    hud.position = {40.0f, 20.0f, 5.0f};
    hud.color = {1.0f, 1.0f, 1.0f, 1.0f};
    const ScreenVertex hud_native = VertexPipeline(through_native).project(VertexPipeline(through_native).transform(hud));
    const ScreenVertex hud_wide = VertexPipeline(through_wide).project(VertexPipeline(through_wide).transform(hud));
    check(close(hud_wide.x, hud_native.x + 240.0f * (k - 1.0f), 1.0e-3f),
          "the HUD is recentred in the widened frame, never stretched");
    check(close(hud_wide.y, hud_native.y), "the HUD keeps its vertical position");

    TransformState off = native;
    off.widescreen = 1.0f;
    const ScreenVertex unchanged = VertexPipeline(off).project(VertexPipeline(off).transform(position_vertex(-1.0f, -1.0f, 0.5f)));
    check(unchanged.x == 0.0f, "widening off is exactly today's mapping");
}

void vfpu_dot_parity() {
    psprecomp::AllegrexContext ctx;
    Mat44 matrix;
    for (int i = 0; i < 16; ++i) matrix.m[i] = static_cast<float>((i * 7) % 11) * 0.25f - 0.75f;
    const Vec4 input{1.0f, -2.0f, 3.0f, 4.0f};
    const Vec4 scalar = matrix.transform(input);

    const auto vfpu_dot = [&ctx](const std::array<float, 4> &a, const std::array<float, 4> &b) {
        ctx.write_vfpu_vector(a.data(), 0u, 4u);
        ctx.write_vfpu_vector(b.data(), 4u, 4u);
        ctx.execute_vfpu_vdot(12u, 0u, 4u, 4u);
        float value[4]{};
        ctx.read_vfpu_vector(value, 12u, 1u);
        return value[0];
    };

    const std::array<float, 4> input_lanes{input.x, input.y, input.z, input.w};
    const float rows[4][4] = {
        {matrix.m[0], matrix.m[4], matrix.m[8], matrix.m[12]},
        {matrix.m[1], matrix.m[5], matrix.m[9], matrix.m[13]},
        {matrix.m[2], matrix.m[6], matrix.m[10], matrix.m[14]},
        {matrix.m[3], matrix.m[7], matrix.m[11], matrix.m[15]},
    };
    const float expected[4] = {scalar.x, scalar.y, scalar.z, scalar.w};
    for (int row = 0; row < 4; ++row) {
        const std::array<float, 4> lanes{rows[row][0], rows[row][1], rows[row][2], rows[row][3]};
        check(close(vfpu_dot(lanes, input_lanes), expected[row]), "VFPU dot matches scalar matrix row");
    }
}

void determinism() {
    const TransformState state = base_state();
    const VertexPipeline first(state);
    const VertexPipeline second(state);
    for (int i = 0; i < 32; ++i) {
        const Vertex vertex = position_vertex(static_cast<float>(i), -static_cast<float>(i), 0.25f);
        const ClipVertex a = first.transform(vertex);
        const ClipVertex b = second.transform(vertex);
        check(a == b, "two pipelines produce identical clip vertices");
        check(first.project(a) == second.project(b), "two pipelines produce identical screen vertices");
    }
}

} // namespace

int main() {
    try {
        vertex_formats();
        transform_and_viewport();
        composition_and_scalar_parity();
        clipping();
        culling();
        lighting_and_material();
        through_and_texture_matrix();
        widescreen_widening();
        vfpu_dot_parity();
        determinism();
        std::cout << "ge_vertex_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

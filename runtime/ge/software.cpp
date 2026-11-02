#include "ge/software.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gungnir::ge {
namespace {

float edge(const ScreenVertex &a, const ScreenVertex &b, float x, float y) {
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

std::uint32_t channel(std::uint32_t color, int shift) {
    return (color >> shift) & 255u;
}

std::uint32_t blend_channel(std::uint32_t source, std::uint32_t destination, BlendFactor source_factor,
                            BlendFactor destination_factor, BlendEquation equation, int shift,
                            std::uint32_t fixed_source, std::uint32_t fixed_destination) {
    const auto fix_byte = [](std::uint32_t fixed, int byte) -> std::uint32_t {
        return (fixed >> (8 * (byte > 2 ? 2 : byte))) & 255u;
    };
    const auto sf = [&](BlendFactor factor, bool is_source) {
        const std::uint32_t fixed = is_source ? fixed_source : fixed_destination;
        switch (factor) {
        case BlendFactor::Zero:
            return 0u;
        case BlendFactor::One:
            return 255u;
        case BlendFactor::SrcColor:
            return channel(source, shift);
        case BlendFactor::OneMinusSrcColor:
            return 255u - channel(source, shift);
        case BlendFactor::DstColor:
            return channel(destination, shift);
        case BlendFactor::OneMinusDstColor:
            return 255u - channel(destination, shift);
        case BlendFactor::SrcAlpha:
            return channel(source, 24);
        case BlendFactor::OneMinusSrcAlpha:
            return 255u - channel(source, 24);
        case BlendFactor::DstAlpha:
            return channel(destination, 24);
        case BlendFactor::OneMinusDstAlpha:
            return 255u - channel(destination, 24);
        case BlendFactor::DoubleSrcAlpha: {
            const std::uint32_t doubled = 2u * channel(source, 24);
            return doubled > 255u ? 255u : doubled;
        }
        case BlendFactor::DoubleOneMinusSrcAlpha: {
            const std::uint32_t doubled = 2u * channel(source, 24);
            return doubled > 255u ? 0u : 255u - doubled;
        }
        case BlendFactor::DoubleDstAlpha: {
            const std::uint32_t doubled = 2u * channel(destination, 24);
            return doubled > 255u ? 255u : doubled;
        }
        case BlendFactor::DoubleOneMinusDstAlpha: {
            const std::uint32_t doubled = 2u * channel(destination, 24);
            return doubled > 255u ? 0u : 255u - doubled;
        }
        case BlendFactor::Fix:
            return shift >= 24 ? 255u : fix_byte(fixed, shift / 8);
        }
        return 0u;
    };
    const std::uint32_t s = channel(source, shift);
    const std::uint32_t d = channel(destination, shift);
    switch (equation) {
    case BlendEquation::Add:
        // Clamp: factors can sum past 1.0 (One, Fix, doubled alpha), and the
        // framebuffer holds 8 bits per channel (both reference impls clamp).
        return std::min(255u, (s * sf(source_factor, true) + d * sf(destination_factor, false)) / 255u);
    case BlendEquation::Subtract:
        return std::max(0, static_cast<int>(s * sf(source_factor, true)) -
                               static_cast<int>(d * sf(destination_factor, false))) /
               255;
    case BlendEquation::ReverseSubtract:
        return std::max(0, static_cast<int>(d * sf(destination_factor, false)) -
                               static_cast<int>(s * sf(source_factor, true))) /
               255;
    case BlendEquation::Min:
        return std::min(s, d);
    case BlendEquation::Max:
        return std::max(s, d);
    }
    return 0u;
}

void mix_byte(std::uint64_t &hash, std::uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
}

void mix_u32(std::uint64_t &hash, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) mix_byte(hash, static_cast<std::uint8_t>(value >> shift));
}

} // namespace

bool compare(CompareFunc function, std::uint32_t incoming, std::uint32_t stored) {
    switch (function) {
    case CompareFunc::Never:
        return false;
    case CompareFunc::Always:
        return true;
    case CompareFunc::Equal:
        return incoming == stored;
    case CompareFunc::NotEqual:
        return incoming != stored;
    case CompareFunc::Less:
        return incoming < stored;
    case CompareFunc::LessEqual:
        return incoming <= stored;
    case CompareFunc::Greater:
        return incoming > stored;
    case CompareFunc::GreaterEqual:
        return incoming >= stored;
    }
    return false;
}

Framebuffer::Framebuffer(int width, int height) : width_(width), height_(height) {
    const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    color.resize(count);
    depth.resize(count);
    stencil.resize(count);
}

void Framebuffer::clear(std::uint32_t clear_color, std::uint16_t clear_depth, std::uint8_t clear_stencil) {
    std::fill(color.begin(), color.end(), clear_color);
    std::fill(depth.begin(), depth.end(), clear_depth);
    std::fill(stencil.begin(), stencil.end(), clear_stencil);
}

std::uint32_t Framebuffer::color_at(int x, int y) const {
    return color[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

std::uint16_t Framebuffer::depth_at(int x, int y) const {
    return depth[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

std::uint8_t Framebuffer::stencil_at(int x, int y) const {
    return stencil[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

std::uint64_t Framebuffer::hash() const {
    std::uint64_t value = 14695981039346656037ull;
    mix_u32(value, static_cast<std::uint32_t>(width_));
    mix_u32(value, static_cast<std::uint32_t>(height_));
    for (const std::uint32_t pixel : color) mix_u32(value, pixel);
    for (const std::uint16_t z : depth) {
        mix_byte(value, static_cast<std::uint8_t>(z & 0xFFu));
        mix_byte(value, static_cast<std::uint8_t>(z >> 8u));
    }
    for (const std::uint8_t s : stencil) mix_byte(value, s);
    return value;
}

SoftwareRasterizer::SoftwareRasterizer(Framebuffer &target, RasterState state)
    : target_(target), state_(state) {
    if (state_.scissor_x1 == 0 && state_.scissor_y1 == 0) {
        state_.scissor_x1 = target_.width();
        state_.scissor_y1 = target_.height();
    }
}

bool SoftwareRasterizer::in_scissor(int x, int y) const {
    return x >= state_.scissor_x0 && x < state_.scissor_x1 && y >= state_.scissor_y0 && y < state_.scissor_y1 &&
           x >= 0 && y >= 0 && x < target_.width() && y < target_.height();
}

void SoftwareRasterizer::shade_pixel(int x, int y, const Fragment &fragment) {
    if (!in_scissor(x, y)) return;
    const std::size_t index =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(target_.width()) + static_cast<std::size_t>(x);

    if (state_.stencil.test &&
        !compare(state_.stencil.function, state_.stencil.reference, target_.stencil[index]))
        return;
    if (state_.stencil.write) target_.stencil[index] = state_.stencil.reference;

    std::uint32_t source = rgba8(to_byte(fragment.color[0]), to_byte(fragment.color[1]), to_byte(fragment.color[2]),
                                 to_byte(fragment.color[3]));
    if (state_.texture_enabled && state_.texture != nullptr) {
        float q = fragment.texcoord[2];
        if (q == 0.0f) q = 1.0f;
        const std::uint32_t texel = state_.texture->sample(fragment.texcoord[0] / q, fragment.texcoord[1] / q);
        source = rgba8(channel(source, 0) * channel(texel, 0) / 255u, channel(source, 8) * channel(texel, 8) / 255u,
                       channel(source, 16) * channel(texel, 16) / 255u,
                       channel(source, 24) * channel(texel, 24) / 255u);
    }

    if (state_.alpha_test && !compare(state_.alpha_function, channel(source, 24), state_.alpha_reference)) return;

    if (state_.fog_enabled) {
        const float clamped = std::clamp(fragment.fog, 0.0f, 1.0f);
        const int fog8 = static_cast<int>(std::lround(clamped * 256.0f));
        if (fog8 < 256) {
            const int fr = static_cast<int>(state_.fog_color & 255u);
            const int fg = static_cast<int>((state_.fog_color >> 8u) & 255u);
            const int fb = static_cast<int>((state_.fog_color >> 16u) & 255u);
            const int sr = static_cast<int>(channel(source, 0));
            const int sg = static_cast<int>(channel(source, 8));
            const int sb = static_cast<int>(channel(source, 16));
            source = rgba8(static_cast<std::uint32_t>(fr + (((sr - fr) * fog8) >> 8)),
                           static_cast<std::uint32_t>(fg + (((sg - fg) * fog8) >> 8)),
                           static_cast<std::uint32_t>(fb + (((sb - fb) * fog8) >> 8)),
                           channel(source, 24));
        }
    }

    const std::uint16_t z =
        static_cast<std::uint16_t>(std::lround(std::clamp(fragment.z, 0.0f, 65535.0f)));
    if (state_.depth_test && !compare(state_.depth_function, z, target_.depth[index])) return;
    if (state_.depth_write) target_.depth[index] = z;

    if (state_.blend.enabled) {
        const std::uint32_t destination = target_.color[index];
        source = rgba8(blend_channel(source, destination, state_.blend.source, state_.blend.destination,
                                     state_.blend.equation, 0, state_.blend.fixed_source,
                                     state_.blend.fixed_destination),
                       blend_channel(source, destination, state_.blend.source, state_.blend.destination,
                                     state_.blend.equation, 8, state_.blend.fixed_source,
                                     state_.blend.fixed_destination),
                       blend_channel(source, destination, state_.blend.source, state_.blend.destination,
                                     state_.blend.equation, 16, state_.blend.fixed_source,
                                     state_.blend.fixed_destination),
                       blend_channel(source, destination, state_.blend.source, state_.blend.destination,
                                     state_.blend.equation, 24, state_.blend.fixed_source,
                                     state_.blend.fixed_destination));
    }
    // Write mask: bits set keep the destination (CLEAR alpha clears and
    // PMASK depth/stencil primes rely on this).
    target_.color[index] = (source & ~state_.write_mask) | (target_.color[index] & state_.write_mask);
}

void SoftwareRasterizer::draw_triangle(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c) {
    const float area = edge(a, b, c.x, c.y);
    if (area == 0.0f) return;
    const bool positive = area > 0.0f;
    const float inverse_area = 1.0f / area;

    const int min_x = std::max(state_.scissor_x0,
                               static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(state_.scissor_x1 - 1,
                               static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int min_y = std::max(state_.scissor_y0,
                               static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int max_y = std::min(state_.scissor_y1 - 1,
                               static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float w0 = edge(b, c, px, py);
            const float w1 = edge(c, a, px, py);
            const float w2 = edge(a, b, px, py);
            if (positive ? (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                         : (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f))
                continue;
            const float l0 = w0 * inverse_area;
            const float l1 = w1 * inverse_area;
            const float l2 = w2 * inverse_area;
            const float inverse_w = l0 * a.inv_w + l1 * b.inv_w + l2 * c.inv_w;
            if (inverse_w == 0.0f) continue;
            const float w = 1.0f / inverse_w;
            Fragment fragment;
            fragment.z = l0 * a.z + l1 * b.z + l2 * c.z;
            fragment.inv_w = inverse_w;
            fragment.fog = l0 * a.fog + l1 * b.fog + l2 * c.fog;
            for (int i = 0; i < 3; ++i)
                fragment.texcoord[i] = (l0 * a.texcoord[i] * a.inv_w + l1 * b.texcoord[i] * b.inv_w +
                                        l2 * c.texcoord[i] * c.inv_w) *
                                       w;
            for (int i = 0; i < 4; ++i)
                fragment.color[i] = (l0 * a.color[i] * a.inv_w + l1 * b.color[i] * b.inv_w +
                                     l2 * c.color[i] * c.inv_w) *
                                    w;
            shade_pixel(x, y, fragment);
        }
    }
}

void SoftwareRasterizer::draw_sprite(const ScreenVertex &top_left, const ScreenVertex &bottom_right) {
    const float min_x = std::min(top_left.x, bottom_right.x);
    const float max_x = std::max(top_left.x, bottom_right.x);
    const float min_y = std::min(top_left.y, bottom_right.y);
    const float max_y = std::max(top_left.y, bottom_right.y);

    ScreenVertex top_right = top_left;
    top_right.x = max_x;
    top_right.y = min_y;
    top_right.texcoord = {bottom_right.texcoord[0], top_left.texcoord[1], bottom_right.texcoord[2]};
    ScreenVertex bottom_left = top_left;
    bottom_left.x = min_x;
    bottom_left.y = max_y;
    bottom_left.texcoord = {top_left.texcoord[0], bottom_right.texcoord[1], bottom_right.texcoord[2]};
    ScreenVertex bottom = bottom_right;
    bottom.x = max_x;
    bottom.y = max_y;
    draw_triangle(top_left, top_right, bottom);
    draw_triangle(top_left, bottom, bottom_left);
}

void SoftwareRasterizer::draw_line(const ScreenVertex &a, const ScreenVertex &b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy)));
    if (steps == 0) {
        draw_point(a);
        return;
    }
    for (int step = 0; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float inverse_w = a.inv_w + (b.inv_w - a.inv_w) * t;
        if (inverse_w == 0.0f) continue;
        const float w = 1.0f / inverse_w;
        Fragment fragment;
        fragment.z = a.z + (b.z - a.z) * t;
        fragment.inv_w = inverse_w;
        fragment.fog = a.fog + (b.fog - a.fog) * t;
        for (int i = 0; i < 3; ++i)
            fragment.texcoord[i] = (a.texcoord[i] * a.inv_w + (b.texcoord[i] * b.inv_w - a.texcoord[i] * a.inv_w) * t) * w;
        for (int i = 0; i < 4; ++i)
            fragment.color[i] = (a.color[i] * a.inv_w + (b.color[i] * b.inv_w - a.color[i] * a.inv_w) * t) * w;
        shade_pixel(static_cast<int>(std::lround(a.x + dx * t)), static_cast<int>(std::lround(a.y + dy * t)),
                    fragment);
    }
}

void SoftwareRasterizer::draw_point(const ScreenVertex &point) {
    Fragment fragment;
    fragment.z = point.z;
    fragment.inv_w = point.inv_w;
    fragment.texcoord = point.texcoord;
    fragment.color = point.color;
    fragment.fog = point.fog;
    shade_pixel(static_cast<int>(std::lround(point.x)), static_cast<int>(std::lround(point.y)), fragment);
}

void SoftwareRasterizer::draw_primitive(Primitive primitive, std::span<const ScreenVertex> vertices) {
    switch (primitive) {
    case Primitive::Points:
        for (const ScreenVertex &vertex : vertices) draw_point(vertex);
        break;
    case Primitive::Lines:
        for (std::size_t i = 0; i + 1u < vertices.size(); i += 2u) draw_line(vertices[i], vertices[i + 1u]);
        break;
    case Primitive::LineStrip:
        for (std::size_t i = 0; i + 1u < vertices.size(); ++i) draw_line(vertices[i], vertices[i + 1u]);
        break;
    case Primitive::Triangles:
        for (std::size_t i = 0; i + 2u < vertices.size(); i += 3u)
            draw_triangle(vertices[i], vertices[i + 1u], vertices[i + 2u]);
        break;
    case Primitive::TriangleStrip:
        for (std::size_t i = 0; i + 2u < vertices.size(); ++i)
            draw_triangle(vertices[i], vertices[i + 1u], vertices[i + 2u]);
        break;
    case Primitive::TriangleFan:
        for (std::size_t i = 1; i + 1u < vertices.size(); ++i)
            draw_triangle(vertices[0], vertices[i], vertices[i + 1u]);
        break;
    case Primitive::Sprites:
        for (std::size_t i = 0; i + 1u < vertices.size(); i += 2u)
            draw_sprite(vertices[i], vertices[i + 1u]);
        break;
    case Primitive::Invalid:
        break;
    }
}

} // namespace gungnir::ge

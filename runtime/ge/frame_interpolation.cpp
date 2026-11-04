#include "ge/frame_interpolation.hpp"

#include <cmath>
#include <utility>

namespace gungnir::ge {
namespace {

// FNV-1a over the fields that make a draw the same draw next frame.
std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value;
    return hash * 1099511628211ull;
}

} // namespace

std::uint64_t draw_key(const Draw &draw) noexcept {
    std::uint64_t hash = 1469598103934665603ull;
    hash = mix(hash, static_cast<std::uint64_t>(draw.primitive));
    hash = mix(hash, static_cast<std::uint64_t>(draw.vertices.size()));
    hash = mix(hash, draw.state.texture_key);
    hash = mix(hash, draw.state.texture_enabled ? 1u : 0u);
    hash = mix(hash, draw.state.draw_format);
    hash = mix(hash, draw.state.through ? 1u : 0u);
    return hash;
}

bool interpolatable(const Draw &draw) noexcept {
    return !draw.state.through && draw.vertices.size() >= 3u;
}

void FrameHistory::push(std::vector<Draw> &&draws) {
    previous_ = std::move(draws);
    by_key_.clear();
    matches_ = 0u;
    for (std::size_t i = 0; i < previous_.size(); ++i)
        by_key_[draw_key(previous_[i])].push_back(static_cast<std::uint32_t>(i));
}

const Draw *FrameHistory::match(const Draw &draw, std::uint32_t occurrence) const noexcept {
    const auto found = by_key_.find(draw_key(draw));
    if (found == by_key_.end() || occurrence >= found->second.size()) return nullptr;
    ++matches_;
    return &previous_[found->second[occurrence]];
}

void interpolate_frame(std::vector<Draw> &frame, const FrameHistory &history, float t,
    InterpolationStats *stats) {
    InterpolationStats local;
    InterpolationStats &counts = stats != nullptr ? *stats : local;
    counts = {};
    std::unordered_map<std::uint64_t, std::uint32_t> occurrence;
    const float cut_pixels = 480.0f * kCutScreenFraction;
    for (Draw &draw : frame) {
        if (!interpolatable(draw)) continue;
        ++counts.candidates;
        const std::uint32_t index = occurrence[draw_key(draw)]++;
        const Draw *previous = history.match(draw, index);
        if (previous == nullptr || !interpolatable(*previous) ||
            previous->vertices.size() != draw.vertices.size()) {
            ++counts.unmatched;
            continue;
        }
        // A partner that moved implausibly far is another object or a camera
        // cut: reject it and keep this frame's vertices untouched.
        float total = 0.0f;
        for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
            total += std::fabs(draw.vertices[i].x - previous->vertices[i].x) +
                std::fabs(draw.vertices[i].y - previous->vertices[i].y);
        }
        if (total / static_cast<float>(draw.vertices.size()) > cut_pixels) {
            ++counts.cut;
            continue;
        }
        for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
            draw.vertices[i].x = previous->vertices[i].x +
                (draw.vertices[i].x - previous->vertices[i].x) * t;
            draw.vertices[i].y = previous->vertices[i].y +
                (draw.vertices[i].y - previous->vertices[i].y) * t;
            draw.vertices[i].z = previous->vertices[i].z +
                (draw.vertices[i].z - previous->vertices[i].z) * t;
            draw.vertices[i].inv_w = previous->vertices[i].inv_w +
                (draw.vertices[i].inv_w - previous->vertices[i].inv_w) * t;
        }
        ++counts.matched;
    }
}

void FrameHistory::clear() {
    previous_.clear();
    by_key_.clear();
    matches_ = 0u;
}

} // namespace gungnir::ge

#include "ge/frame_interpolation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gungnir::ge {
namespace {

// FNV-1a over the fields that make a draw the same draw next frame.
std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    hash ^= value;
    return hash * 1099511628211ull;
}

struct Center {
    float x{};
    float y{};
};

Center center_of(const std::vector<ScreenVertex> &vertices) noexcept {
    if (vertices.empty()) return {};
    float sum_x = 0.0f;
    float sum_y = 0.0f;
    for (const ScreenVertex &vertex : vertices) {
        sum_x += vertex.x;
        sum_y += vertex.y;
    }
    const float count = static_cast<float>(vertices.size());
    return {sum_x / count, sum_y / count};
}

float separation(const Center &a, const Center &b) noexcept {
    return std::fabs(a.x - b.x) + std::fabs(a.y - b.y);
}

// Signed area of the triangle a vertex and its two successors form. Screen space
// is y-down, so the sign carries the winding.
float triangle_area(const std::vector<ScreenVertex> &vertices, std::size_t first) noexcept {
    const ScreenVertex &a = vertices[first];
    const ScreenVertex &b = vertices[first + 1u];
    const ScreenVertex &c = vertices[first + 2u];
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
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
    // Screen-space (through) draws are candidates too. Gungnir projects its
    // battle unit sprites on the CPU and submits them that way, so excluding the
    // class left every unit stepping at the guest rate while the world behind
    // them was smoothed: the reported sprite shake. Identity and position decide
    // what is the same draw; the transform class does not.
    return draw.vertices.size() >= 3u;
}

void FrameHistory::push(std::vector<Draw> &&draws) {
    previous_ = std::move(draws);
    by_key_.clear();
    center_x_.resize(previous_.size());
    center_y_.resize(previous_.size());
    matches_ = 0u;
    for (std::size_t i = 0; i < previous_.size(); ++i) {
        const Center center = center_of(previous_[i].vertices);
        center_x_[i] = center.x;
        center_y_[i] = center.y;
        by_key_[draw_key(previous_[i])].push_back(static_cast<std::uint32_t>(i));
    }
}

const Draw *FrameHistory::match(const Draw &draw, std::uint32_t occurrence) const noexcept {
    const auto found = by_key_.find(draw_key(draw));
    if (found == by_key_.end() || occurrence >= found->second.size()) return nullptr;
    ++matches_;
    return &previous_[found->second[occurrence]];
}

const std::vector<std::uint32_t> *FrameHistory::bucket(std::uint64_t key) const noexcept {
    const auto found = by_key_.find(key);
    return found == by_key_.end() ? nullptr : &found->second;
}

float FrameHistory::center_x(std::uint32_t index) const noexcept {
    return index < center_x_.size() ? center_x_[index] : 0.0f;
}

float FrameHistory::center_y(std::uint32_t index) const noexcept {
    return index < center_y_.size() ? center_y_[index] : 0.0f;
}

void interpolate_frame(std::vector<Draw> &frame, const FrameHistory &history, float t,
    InterpolationStats *stats) {
    InterpolationStats local;
    InterpolationStats &counts = stats != nullptr ? *stats : local;
    counts = {};
    if (frame.empty()) return;
    const float relative_pixels = 480.0f * kCutScreenFraction;
    const float frame_cut_pixels = 480.0f * kFrameCutFraction;
    constexpr std::uint32_t kNoSlot = std::numeric_limits<std::uint32_t>::max();

    // Group this frame's candidates by identity, so a partner can be chosen by
    // where a draw is rather than by when it arrived. Gungnir submits one
    // character's sprite as several quads of one sheet, and both a cursor
    // highlight and an animation frame add or drop a draw of that sheet; with
    // arrival order every later quad then pairs with its neighbour and the
    // sprite shakes apart.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> buckets;
    std::vector<Center> center(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (!interpolatable(frame[i])) continue;
        ++counts.candidates;
        buckets[draw_key(frame[i])].push_back(static_cast<std::uint32_t>(i));
        center[i] = center_of(frame[i].vertices);
    }

    // Which previous draw each candidate took, as a slot inside its bucket.
    std::vector<std::uint32_t> chosen(frame.size(), kNoSlot);

    struct Pair {
        float distance;
        std::uint32_t current;
        std::uint32_t slot;
    };
    std::vector<Pair> pairs;
    for (const auto &[key, current] : buckets) {
        const std::vector<std::uint32_t> *previous = history.bucket(key);
        if (previous == nullptr) continue;
        counts.largest_bucket = std::max<std::size_t>(counts.largest_bucket, current.size());
        if (current.size() > kMaxBucketScan || previous->size() > kMaxBucketScan) {
            // Repetitive scenery: arrival order is correct there (each tile is
            // equidistant from its own previous position and its neighbour's, so
            // position matching slides the pattern) and the scan would be
            // quadratic. Counted, never silent.
            ++counts.oversize;
            counts.oversize_draws += current.size();
            const std::size_t shared = std::min(current.size(), previous->size());
            for (std::size_t order = 0; order < shared; ++order)
                chosen[current[order]] = static_cast<std::uint32_t>(order);
            continue;
        }
        // Closest pairs first, so a surplus draw stays unmatched rather than
        // stealing the partner of a draw that really did correspond.
        pairs.clear();
        for (std::uint32_t c : current) {
            for (std::size_t slot = 0; slot < previous->size(); ++slot) {
                const std::uint32_t p = (*previous)[slot];
                const Center other{history.center_x(p), history.center_y(p)};
                pairs.push_back({separation(center[c], other), c,
                    static_cast<std::uint32_t>(slot)});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const Pair &a, const Pair &b) {
            if (a.distance != b.distance) return a.distance < b.distance;
            if (a.current != b.current) return a.current < b.current;
            return a.slot < b.slot;
        });
        std::vector<char> taken(previous->size(), 0);
        for (const Pair &pair : pairs) {
            if (chosen[pair.current] != kNoSlot || taken[pair.slot] != 0) continue;
            taken[pair.slot] = 1;
            chosen[pair.current] = pair.slot;
        }
        for (std::size_t order = 0; order < current.size(); ++order) {
            const std::uint32_t slot = chosen[current[order]];
            if (slot == kNoSlot || slot == order) continue;
            ++counts.reassigned;
            // How far arrival order would have dragged this draw: the partner
            // it used to pick is the previous draw in the same slot.
            if (order >= previous->size()) continue;
            const Draw &draw = frame[current[order]];
            const Draw &arrival = history.previous()[(*previous)[order]];
            if (arrival.vertices.size() != draw.vertices.size()) continue;
            float total = 0.0f;
            for (std::size_t v = 0; v < draw.vertices.size(); ++v) {
                total += std::fabs(draw.vertices[v].x - arrival.vertices[v].x) +
                    std::fabs(draw.vertices[v].y - arrival.vertices[v].y);
            }
            if (total / static_cast<float>(draw.vertices.size()) > kDisplacedPixels)
                ++counts.displaced;
        }
    }

    // How far each pair moves, and how fast the frame itself is moving: a pair
    // is only implausible relative to that. A still scene keeps the absolute
    // margin, a panning scene moves as one instead of leaving its fastest
    // elements behind.
    std::vector<float> motion(frame.size(), -1.0f);
    std::vector<float> matched_motion;
    matched_motion.reserve(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const Draw &draw = frame[i];
        if (!interpolatable(draw)) continue;
        const std::uint32_t slot = chosen[i];
        const std::vector<std::uint32_t> *previous_bucket = history.bucket(draw_key(draw));
        if (slot == kNoSlot || previous_bucket == nullptr || slot >= previous_bucket->size()) {
            ++counts.unmatched;
            continue;
        }
        const Draw &previous = history.previous()[(*previous_bucket)[slot]];
        if (!interpolatable(previous) || previous.vertices.size() != draw.vertices.size()) {
            ++counts.unmatched;
            continue;
        }
        float total = 0.0f;
        for (std::size_t v = 0; v < draw.vertices.size(); ++v) {
            total += std::fabs(draw.vertices[v].x - previous.vertices[v].x) +
                std::fabs(draw.vertices[v].y - previous.vertices[v].y);
        }
        motion[i] = total / static_cast<float>(draw.vertices.size());
        matched_motion.push_back(motion[i]);
    }
    float frame_motion = 0.0f;
    if (!matched_motion.empty()) {
        const std::size_t middle = matched_motion.size() / 2u;
        std::nth_element(matched_motion.begin(), matched_motion.begin() + static_cast<std::ptrdiff_t>(middle),
                         matched_motion.end());
        frame_motion = matched_motion[middle];
    }
    counts.frame_motion = frame_motion;
    const bool frame_cut = frame_motion > frame_cut_pixels;
    const float cut_pixels = frame_motion * kMotionSlack + relative_pixels;

    for (std::size_t i = 0; i < frame.size(); ++i) {
        Draw &draw = frame[i];
        if (!interpolatable(draw) || motion[i] < 0.0f) continue;
        const std::uint32_t slot = chosen[i];
        const std::vector<std::uint32_t> *previous_bucket = history.bucket(draw_key(draw));
        const Draw &previous = history.previous()[(*previous_bucket)[slot]];
        // A partner that moved far more than the frame itself did is another
        // object or a teleport; a frame where everything moved far is a cut.
        // Judging a draw against its own bucket was tried and withdrawn: a door
        // opening in an otherwise static bucket is a fast member, not a mistake.
        if (frame_cut || motion[i] > cut_pixels) {
            ++counts.cut;
            continue;
        }
        // A draw whose shape changed between the frames -- a particle mid-spin,
        // a banner that stretches -- cannot have its vertices moved
        // independently: blending a quad with its rotated self shrinks it, and
        // near 180 degrees it collapses to a line. On the battle snapshot that
        // collapsed 11-29 triangles per frame pair, all of them effect geometry.
        // Such a draw is translated rigidly instead, which keeps the motion and
        // the shape.
        float mean_x = 0.0f;
        float mean_y = 0.0f;
        for (std::size_t v = 0; v < draw.vertices.size(); ++v) {
            mean_x += draw.vertices[v].x - previous.vertices[v].x;
            mean_y += draw.vertices[v].y - previous.vertices[v].y;
        }
        const float count = static_cast<float>(draw.vertices.size());
        mean_x /= count;
        mean_y /= count;
        float deviation = 0.0f;
        for (std::size_t v = 0; v < draw.vertices.size(); ++v) {
            deviation = std::max(deviation,
                std::fabs(draw.vertices[v].x - previous.vertices[v].x - mean_x) +
                std::fabs(draw.vertices[v].y - previous.vertices[v].y - mean_y));
        }
        // The decision is the collapse itself: if the vertex-wise blend would
        // halve or invert any triangle, the draw is moved rigidly instead. A
        // per-vertex deviation test is NOT used -- perspective foreshortening
        // moves a mesh's corners by different amounts without changing its shape,
        // and translating such a mesh rigidly is what made doors and level
        // geometry misbehave (measured: 468-975 meshes per frame pair during a
        // pan). Deviation only distinguishes "the corners disagree", which is
        // exactly what perspective does.
        bool collapses = false;
        {
            for (std::size_t v = 0; v + 2u < draw.vertices.size(); ++v) {
                const float before = triangle_area(previous.vertices, v);
                const float after = triangle_area(draw.vertices, v);
                const float smallest = std::min(std::fabs(before), std::fabs(after));
                if (smallest < 0.5f) continue;
                const auto blend_point = [&](std::size_t index) {
                    const ScreenVertex &now = draw.vertices[index];
                    const ScreenVertex &was = previous.vertices[index];
                    return ScreenVertex{(now.x + was.x) * 0.5f, (now.y + was.y) * 0.5f};
                };
                std::vector<ScreenVertex> blended{blend_point(v), blend_point(v + 1u),
                                                  blend_point(v + 2u)};
                const float middle = triangle_area(blended, 0u);
                if (std::fabs(middle) < 0.5f * smallest ||
                    (middle >= 0.0f) != (before >= 0.0f)) {
                    collapses = true;
                    break;
                }
            }
        }
        const bool translate_only = collapses;
        if (translate_only) ++counts.translated;
        for (std::size_t v = 0; v < draw.vertices.size(); ++v) {
            ScreenVertex &now = draw.vertices[v];
            const ScreenVertex &was = previous.vertices[v];
            if (!translate_only) {
                // Perspective division is not affine: blend (position * w) and w,
                // then divide. Screen x/y blended affinely while inv_w was blended
                // separately would leave a position that corresponds to no 3D
                // point, which is what makes an inclined mesh swim between frames.
                const float w_prev = was.inv_w > 0.0f ? 1.0f / was.inv_w : 0.0f;
                const float w_now = now.inv_w > 0.0f ? 1.0f / now.inv_w : 0.0f;
                if (w_prev > 0.0f && w_now > 0.0f) {
                    const float w = w_prev + (w_now - w_prev) * t;
                    const float x = ((1.0f - t) * was.x * w_prev + t * now.x * w_now) / w;
                    const float y = ((1.0f - t) * was.y * w_prev + t * now.y * w_now) / w;
                    now.x = x;
                    now.y = y;
                    now.inv_w = 1.0f / w;
                    continue;
                }
            }
            const float dx = translate_only ? mean_x : now.x - was.x;
            const float dy = translate_only ? mean_y : now.y - was.y;
            now.x -= dx * (1.0f - t);
            now.y -= dy * (1.0f - t);
            // Depth is left as this frame produced it. It is an ordering key,
            // not a position: blending it invents a relationship neither frame
            // had, and coplanar geometry (a wall and its trim, a tile and its
            // decal) relies on exact equality under the GE's GEQUAL test. On the
            // battle snapshot 432 overlapping pairs change depth-equality class
            // in the blend while agreeing frame to frame.
            draw.vertices[v].inv_w = previous.vertices[v].inv_w +
                (draw.vertices[v].inv_w - previous.vertices[v].inv_w) * t;
        }
        ++counts.matched;
    }
}

void FrameHistory::clear() {
    previous_.clear();
    center_x_.clear();
    center_y_.clear();
    by_key_.clear();
    matches_ = 0u;
}

float scene_motion(const std::vector<Draw> &current,
    const std::vector<Draw> &previous) noexcept {
    // Arrival-order pairing is enough for a whole-scene stillness check: a
    // static frame reproduces its predecessor draw-for-draw, so any partner
    // scheme would report the same zero. Draws added or removed (a changed
    // vertex count) are not motion -- they are unmatched and skipped, so a
    // growing/culling stream does not look like a pan.
    double total = 0.0;
    std::size_t vertices = 0u;
    const std::size_t pairs = std::min(current.size(), previous.size());
    for (std::size_t i = 0u; i < pairs; ++i) {
        const Draw &now = current[i];
        const Draw &was = previous[i];
        if (now.vertices.size() != was.vertices.size()) continue;
        for (std::size_t v = 0u; v < now.vertices.size(); ++v) {
            total += std::fabs(now.vertices[v].x - was.vertices[v].x) +
                std::fabs(now.vertices[v].y - was.vertices[v].y);
            ++vertices;
        }
    }
    return vertices == 0u ? 0.0f : static_cast<float>(total / static_cast<double>(vertices));
}

bool scene_is_static(const std::vector<Draw> &current,
    const std::vector<Draw> &previous) noexcept {
    return scene_motion(current, previous) < kStaticScenePixels;
}

bool frames_identical(const std::vector<Draw> &a,
    const std::vector<Draw> &b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0u; i < a.size(); ++i) {
        const Draw &x = a[i];
        const Draw &y = b[i];
        if (draw_key(x) != draw_key(y)) return false;
        if (x.vertices != y.vertices) return false;
        if (x.indices != y.indices) return false;
    }
    return true;
}

} // namespace gungnir::ge

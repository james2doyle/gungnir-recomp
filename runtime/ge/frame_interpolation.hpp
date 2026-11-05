#pragma once

// Draw identity and the previous-frame record (T17a.4). Renderer interpolation
// needs to know which draw of this frame corresponds to which draw of the last
// one, and what its vertices were.
//
// Identity is what a draw *is*, not where it landed: the target buffer address
// is deliberately excluded, because the game alternates display buffers and an
// address-based key matches nothing (Chinatown Wars hit exactly this and
// documented it). The previous frame's draws are kept as-is; nothing here
// touches guest RAM, a snapshot or a save.

#include "ge/state_machine.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gungnir::ge {

// Stable across frames for the same draw: primitive, vertex count, texture
// identity, target format and transform class.
[[nodiscard]] std::uint64_t draw_key(const Draw &draw) noexcept;

// A draw's identity within its frame: the key plus which occurrence of that key
// it is. Arrival order is only a tie-break for the interpolator, which pairs
// draws of one identity by position first (see interpolate_frame).
struct DrawIdentity {
    std::uint64_t key{};
    std::uint32_t occurrence{};
    bool operator==(const DrawIdentity &) const = default;
};

// A draw is a candidate when it has a triangle. Screen-space (through) draws are
// not excluded: Gungnir submits its battle unit sprites that way, so treating
// the class as un-authorable made the units step at the guest rate while the
// world was smoothed. A draw whose identity changed between frames has no
// partner and is shown as produced, which is what keeps HUD text from smearing.
[[nodiscard]] bool interpolatable(const Draw &draw) noexcept;

// The previous frame's draws, indexed by key, with the lookup the interpolator
// uses. Derived from the draw stream only: never exported, never restored, so
// a snapshot load or a session restart starts with no history.
class FrameHistory {
public:
    // Replaces the previous frame with this one and rebuilds the index.
    void push(std::vector<Draw> &&draws);
    [[nodiscard]] bool empty() const noexcept { return previous_.empty(); }
    [[nodiscard]] const std::vector<Draw> &previous() const noexcept { return previous_; }
    // The matching draw of the previous frame by arrival order, or null when
    // there is none. Only a tie-break for the interpolator: interpolate_frame
    // pairs by position inside the bucket, because arrival order shifts when
    // the game adds or drops a draw of the same identity.
    [[nodiscard]] const Draw *match(const Draw &draw, std::uint32_t occurrence) const noexcept;
    // The previous draws that share this identity, in arrival order. Null when
    // the identity is new.
    [[nodiscard]] const std::vector<std::uint32_t> *bucket(std::uint64_t key) const noexcept;
    // Mean vertex position of a previous draw, precomputed at push.
    [[nodiscard]] float center_x(std::uint32_t index) const noexcept;
    [[nodiscard]] float center_y(std::uint32_t index) const noexcept;
    // How many lookups found a partner since the last push (diagnostics).
    [[nodiscard]] std::size_t matches() const noexcept { return matches_; }
    void clear();

private:
    std::vector<Draw> previous_;
    std::vector<float> center_x_;
    std::vector<float> center_y_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key_;
    mutable std::size_t matches_{};
};

// What one interpolation pass did, for the periodic log.
struct InterpolationStats {
    std::size_t candidates{};  // transform draws in the frame
    std::size_t matched{};     // and how many had a usable partner
    std::size_t cut{};         // partners rejected as a jump (camera cut)
    float frame_motion{};      // median matched motion this frame, guest px
    std::size_t unmatched{};   // no partner at all
    std::size_t reassigned{};  // partners chosen by position, not arrival order
    std::size_t displaced{};   // of those, moved far enough to be visible
    std::size_t oversize{};    // buckets past kMaxBucketScan, matched by arrival
    std::size_t oversize_draws{};  // draws those buckets hold
    std::size_t translated{};  // pairs moved rigidly instead of vertex-wise
    std::size_t largest_bucket{};  // draws in the biggest same-identity bucket
};

// Mean vertex motion, in the 480-wide guest frame, above which a partner the
// arrival-order matcher would have chosen is far enough away to be a visible
// drag rather than a rounding difference. Diagnostics only.
inline constexpr float kDisplacedPixels = 2.0f;

// Largest same-identity bucket that is matched by position. Past this the draws
// are matched by arrival order instead, counted as `oversize` so the limit is
// visible in the log rather than silent. On the battle snapshot one bucket holds
// 546 draws: the map's tiles of one texture. Scenery like that is repetitive, so
// arrival order is both correct and the only affordable choice; matching it by
// position slides the whole ground by one tile whenever the pan moves further
// than the tile spacing.
inline constexpr std::size_t kMaxBucketScan = 64u;

// A still scene's tolerance: average screen-space motion, in the 480-wide guest
// frame, above which a matched pair is unrelated rather than the same object
// moving. This is the original 0.2 of the frame and is deliberately unchanged:
// lowering it to 0.1 (to stop a scrolling list's wrapped item being dragged) made
// doors and other world elements snap where they used to blend, which is a
// regression. Rejection may only ever be *relaxed* from here -- see kMotionSlack.
inline constexpr float kCutScreenFraction = 0.2f;

// How much faster than the frame's own median motion a single pair may move
// before it is treated as unrelated. Screen motion scales with depth, so during
// a pan the foreground legitimately moves several times faster than the map
// behind it: measured on the battle snapshot, a pan with a 14 px median has
// foreground elements at 169 px. A flat distance limit rejected those and made
// them jump every present while everything else glided -- the "level elements
// flickering" report. Scaling with the frame keeps a still scene at the 0.2
// margin, where a cursor or highlight that teleports is still rejected.
inline constexpr float kMotionSlack = 12.0f;

// Median motion above which the whole frame is a camera cut: half a screen in
// one guest interval (ten screens a second). A pan cannot reach it, so a frame
// that does is a scene change and nothing in it is blended.
inline constexpr float kFrameCutFraction = 0.5f;

// Moves `frame`'s matched draws to `t` of the way from the previous frame to
// this one, in place: positions only (x, y, inv_w), never depth, colour,
// texture coordinates or state. Depth is not a position: it decides occlusion,
// and blending it breaks the exact equality coplanar geometry depends on. Through/2D draws, unmatched draws and pairs
// whose vertices jumped are left exactly as the guest produced them.
//
// Inside an identity bucket a partner is the previous draw of that identity
// nearest this one, not the draw at the same arrival index: the battle submits
// one character's sprite as several quads of one sheet, and the order inside a
// bucket is not stable from frame to frame (it changes with a cursor highlight,
// an animation frame, and which sprites the game culled), so index matching
// blends each part toward its neighbour and the sprite shakes apart. Buckets
// past kMaxBucketScan keep arrival order instead, because that is the only
// correct choice for repetitive scenery and the scan would be quadratic.
void interpolate_frame(std::vector<Draw> &frame, const FrameHistory &history, float t,
    InterpolationStats *stats = nullptr);

// Mean per-vertex screen-space motion between two consecutive guest frames,
// matched by arrival order (draws whose vertex count changed have no partner and
// are skipped). Cheap enough to run once per guest frame: one pass, no bucket or
// partner machinery. Zero for two identical frames. Used to decide whether a
// scene is static and therefore needs no in-between presents at all -- a static
// scene has nothing to interpolate, so generating them only burns power.
[[nodiscard]] float scene_motion(const std::vector<Draw> &current,
    const std::vector<Draw> &previous) noexcept;

// A scene this still is treated as static. Sub-pixel drift (a blinking cursor,
// rounding in a fixed camera) stays below it; a real pan does not. A lone small
// moving element in an otherwise still scene is held at the guest frame rate --
// an intentional trade: smoothing one cursor is not worth driving the display at
// its refresh rate for the whole frame.
inline constexpr float kStaticScenePixels = 1.0f;

[[nodiscard]] bool scene_is_static(const std::vector<Draw> &current,
    const std::vector<Draw> &previous) noexcept;

// Exact, cheap comparison of two guest frames for the "hold the last image"
// power optimisation: same draw count, and for each draw the same identity
// (draw_key), the same vertices (bit-exact ScreenVertex equality) and the same
// indices. State fields outside draw_key are deliberately NOT compared -- a
// named heuristic, made safe by the caller's periodic re-present: any change
// we miss is shown within one safety interval. Never edits either frame.
[[nodiscard]] bool frames_identical(const std::vector<Draw> &a,
    const std::vector<Draw> &b) noexcept;

} // namespace gungnir::ge

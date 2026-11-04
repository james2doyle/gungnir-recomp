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
// it is. Repeated draws (tiles, sprites) keep their order instead of matching
// each other at random.
struct DrawIdentity {
    std::uint64_t key{};
    std::uint32_t occurrence{};
    bool operator==(const DrawIdentity &) const = default;
};

// Through-mode draws carry screen-space vertices the game authors per frame
// (menus, text, cursors, HUD): an in-between would smear them.
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
    // The matching draw of the previous frame, or null when there is none.
    [[nodiscard]] const Draw *match(const Draw &draw, std::uint32_t occurrence) const noexcept;
    // How many lookups found a partner since the last push (diagnostics).
    [[nodiscard]] std::size_t matches() const noexcept { return matches_; }
    void clear();

private:
    std::vector<Draw> previous_;
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> by_key_;
    mutable std::size_t matches_{};
};

} // namespace gungnir::ge

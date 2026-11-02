#pragma once

// GE render targets: route draws by guest draw buffer (FBP 0x9C offset)
// into per-target surfaces, select the display target on flip, and sample
// target surfaces for render-to-texture reads.

#include "ge/software.hpp"
#include "ge/texture.hpp"

#include <cstdint>
#include <map>
#include <optional>

namespace gungnir::ge {

// PSP VRAM base: draw-buffer offsets translate to full guest addresses here.
constexpr std::uint32_t kVramBase = 0x04000000u;

struct TargetEntry {
    Framebuffer surface;
    bool dirty = false;
};

class TargetSet {
public:
    TargetSet(int width, int height);

    // Surface for a draw-buffer offset, created on demand. The caller
    // marks entry.dirty after rasterizing into surface.
    TargetEntry &draw_to(std::uint32_t draw_buffer);
    [[nodiscard]] bool has(std::uint32_t draw_buffer) const;
    // Display surface: the target matching the display offset, else
    // target 0, else the first surface, else nullptr.
    [[nodiscard]] const Framebuffer *display(std::uint32_t display_offset) const;
    // RTT: sample a dirty target overlapping tex_addr when the texture
    // params describe the surface exactly (RGBA8888, matching extent).
    // Anything else returns nullopt and the caller decodes from RAM.
    [[nodiscard]] std::optional<CpuTexture> sample_target(std::uint32_t tex_addr,
                                                          const TextureState &texture) const;

private:
    int width_;
    int height_;
    std::map<std::uint32_t, TargetEntry> targets_;
};

} // namespace gungnir::ge

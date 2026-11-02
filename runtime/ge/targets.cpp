#include "ge/targets.hpp"

#include <cstring>

namespace gungnir::ge {

TargetSet::TargetSet(int width, int height) : width_(width), height_(height) {}

TargetEntry &TargetSet::draw_to(std::uint32_t draw_buffer, TextureFormat format) {
    auto found = targets_.find(draw_buffer);
    if (found != targets_.end()) return found->second;
    TargetEntry &entry = targets_.emplace(draw_buffer, TargetEntry{Framebuffer(width_, height_)}).first->second;
    entry.format = format;
    // Fresh surfaces start cleared opaque black, matching the legacy
    // single-framebuffer render path.
    entry.surface.clear(0xFF000000u);
    return entry;
}

bool TargetSet::has(std::uint32_t draw_buffer) const {
    return targets_.find(draw_buffer) != targets_.end();
}

const Framebuffer *TargetSet::display(std::uint32_t display_offset) const {
    if (const auto found = targets_.find(display_offset); found != targets_.end())
        return &found->second.surface;
    if (const auto zero = targets_.find(0u); zero != targets_.end()) return &zero->second.surface;
    if (!targets_.empty()) return &targets_.begin()->second.surface;
    return nullptr;
}

std::optional<CpuTexture> TargetSet::sample_target(std::uint32_t tex_addr,
                                                   const TextureState &texture) const {
    if (texture.format != TextureFormat::Rgba8888) return std::nullopt;
    for (const auto &[buffer, entry] : targets_) {
        if (!entry.dirty) continue;
        const std::uint32_t base = kVramBase + buffer;
        const std::uint32_t size =
            static_cast<std::uint32_t>(entry.surface.width() * entry.surface.height() * 4);
        if (tex_addr < base || tex_addr >= base + size) continue;
        // The surface can only stand in for the texture when the texture
        // params describe it exactly; anything else decodes from RAM.
        if (texture.width != entry.surface.width() || texture.height != entry.surface.height() ||
            texture.buffer_width != entry.surface.width())
            return std::nullopt;
        CpuTexture sampled;
        sampled.data.resize(entry.surface.color.size() * sizeof(std::uint32_t));
        std::memcpy(sampled.data.data(), entry.surface.color.data(), sampled.data.size());
        sampled.texture.emplace(texture, std::span<const std::uint8_t>(sampled.data),
                                        std::span<const std::uint8_t>());
        return sampled;
    }
    return std::nullopt;
}

} // namespace gungnir::ge

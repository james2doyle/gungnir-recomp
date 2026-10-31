// sceAtrac3plus: object lifetime first; track parsing and decode follow the
// calls the title actually makes (boot depth), never a silent zero.
#include "hle/hle_atrac.hpp"

#include "hle/kernel.hpp"

#include <cstdint>
#include <map>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kCodecAt3Plus = 0x00001000u;
constexpr std::uint32_t kCodecAt3 = 0x00001001u;
constexpr std::uint32_t kBadCodecType = 0x80630004u;
constexpr std::uint32_t kBadAtracId = 0x80630005u;

struct AtracObject {
    std::uint32_t codec{};
};

std::map<std::int32_t, AtracObject> g_objects;
std::int32_t g_next_id{1};

void finish(AllegrexContext &ctx, std::uint32_t result) {
    kernel().finish(ctx, result);
}

} // namespace

void reset_atrac_state() {
    g_objects.clear();
    g_next_id = 1;
}

void register_atrac(HleRegistrar &hle) {
    hle.add("sceAtrac3plus", "sceAtracGetAtracID", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t codec = arg(ctx, 0);
        if (codec != kCodecAt3Plus && codec != kCodecAt3) {
            finish(ctx, kBadCodecType);
            return;
        }
        const std::int32_t id = g_next_id++;
        g_objects[id] = AtracObject{codec};
        finish(ctx, static_cast<std::uint32_t>(id));
    });
    hle.add("sceAtrac3plus", "sceAtracReleaseAtracID", [](Runtime &, AllegrexContext &ctx) {
        const auto found = g_objects.find(static_cast<std::int32_t>(arg(ctx, 0)));
        if (found == g_objects.end()) {
            finish(ctx, kBadAtracId);
            return;
        }
        g_objects.erase(found);
        finish(ctx, 0u);
    });
}

} // namespace gungnir::hle

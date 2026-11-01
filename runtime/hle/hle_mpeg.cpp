#include "hle/hle_mpeg.hpp"

#include "psprecomp/common.hpp"

#include <string>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kMpegNoRapi = 0x80618004u;
constexpr std::uint32_t kMpegNotInitialized = 0x80618009u;

void note_gap(const char *name) {
    log_once(std::string("mpeg:") + name,
             std::string("[hle] gap: sceMpeg::") + name + " unsupported (movie playback deferred)");
}

} // namespace

void register_mpeg(HleRegistrar &hle) {
    hle.add("sceMpeg", "sceMpegInit", [](Runtime &, AllegrexContext &ctx) {
        note_gap("sceMpegInit");
        kernel().finish(ctx, kMpegNoRapi);
    });
    for (const char *name : {"sceMpegFinish", "sceMpegDelete", "sceMpegRingbufferDestruct"}) {
        hle.add("sceMpeg", name, [name](Runtime &, AllegrexContext &ctx) {
            note_gap(name);
            kernel().finish(ctx, 0u);
        });
    }
    for (const char *name : {"sceMpegAvcDecode", "sceMpegAvcDecodeDetail", "sceMpeg_11CAB459",
             "sceMpegInitAu", "sceMpegQueryStreamOffset",
             "sceMpegChangeGetAvcAuMode", "sceMpegRingbufferConstruct", "sceMpegNextAvcRpAu",
             "sceMpegRegistStream", "sceMpegAvcDecodeFlush", "sceMpegFlushStream",
             "sceMpegUnRegistStream", "sceMpegQueryStreamSize", "sceMpegFlushAllStream",
             "sceMpegAvcDecodeStop", "sceMpegAtracDecode", "sceMpegGetPcmAu", "sceMpeg_988E9E12",
             "sceMpegChangeGetAuMode", "sceMpegAvcDecodeMode", "sceMpegMallocAvcEsBuf",
             "sceMpegRingbufferPut", "sceMpeg_B27711A8", "sceMpegRingbufferAvailableSize",
             "sceMpegQueryPcmEsSize", "sceMpegQueryMemSize", "sceMpeg_C345DED2",
             "sceMpegFreeAvcEsBuf", "sceMpegAvcDecodeDetail2", "sceMpeg_D4DD6E75",
             "sceMpegRingbufferQueryMemSize", "sceMpegCreate", "sceMpegGetAtracAu",
             "sceMpegQueryAtracEsSize", "sceMpegGetAvcAu"}) {
        hle.add("sceMpeg", name, [name](Runtime &, AllegrexContext &ctx) {
            note_gap(name);
            kernel().finish(ctx, kMpegNotInitialized);
        });
    }
}

} // namespace gungnir::hle

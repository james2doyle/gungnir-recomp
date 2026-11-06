#pragma once

// On-device preparation of the game's executable from the encrypted
// EBOOT.BIN on the user's own disc (Yakumo-style installer, ported from
// Yakumo's executable_preparation behaviour; see docs/SOURCE_PROVENANCE.md).
//
// The file is an 0x150-byte ~PSP header followed by the executable encrypted
// with AES-128-CBC under a per-file key. Only the public PSP key tables are
// used (KIRK slot 0x5D + KIRK1), the same long-published material Yakumo and
// pspdecrypt use; no user keys are shipped.
//
// Identity is verified without publishing dump hashes (see docs/LEGAL.md §5):
// callers check the disc (size+CRC, DISC_ID) before calling this, and the
// result is checked to be an ELF here.

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace gungnir::platform {

// Returns the decrypted executable bytes (the ELF). Throws std::runtime_error
// (via psprecomp::Error) with a player-actionable message on any mismatch.
[[nodiscard]] std::vector<std::uint8_t> prepare_executable(
    std::span<const std::uint8_t> eboot_bin,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress = {});

// Identity facts for the one release this project supports (public, from the
// redump disc identity and the open PSP format facts; no dump hashes).
inline constexpr const char *kGungnirDiscId = "ULUS10592";
inline constexpr const char *kGungnirExecutablePathOnDisc = "PSP_GAME/SYSDIR/EBOOT.BIN";
inline constexpr const char *kGungnirParamSfoPathOnDisc = "PSP_GAME/PARAM.SFO";

} // namespace gungnir::platform

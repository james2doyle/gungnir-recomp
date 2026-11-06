#pragma once

// First-run installer (Yakumo-style, adapted): check the player's disc image
// and prepare the game's executable from it, so picking an ISO is enough to
// start the game. The logic knows nothing about how questions are asked;
// the Android shell (SAF picker + JNI) drives it.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gungnir::platform {

// A problem the player can act on; what() is written for them.
class InstallError final : public std::runtime_error {
public:
    explicit InstallError(const std::string &message) : std::runtime_error(message) {}
};

struct InstallInfo {
    std::uint64_t size_bytes{};
    std::string disc_id;
};

// Checks the disc image: ISO9660 readable, PSP_GAME/PARAM.SFO DISC_ID matches
// ULUS10592, PSP_GAME/SYSDIR/EBOOT.BIN present and its ~PSP header is the
// supported tag. Throws InstallError otherwise.
[[nodiscard]] InstallInfo check_image(const std::filesystem::path &path);

// Extracts EBOOT.BIN from the ISO and returns its bytes.
[[nodiscard]] std::vector<std::uint8_t> read_eboot(const std::filesystem::path &path);

// Prepares game.elf from the ISO into `data_dir` (the files/game dir).
// Writes EBOOT.ELF bytes to data_dir/"game.elf" atomically (.part + rename).
// Returns the game.elf path. Throws InstallError on any failure.
std::filesystem::path install_from_iso(
    const std::filesystem::path &iso_path, const std::filesystem::path &data_dir,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress = {});

// Same, from an already-extracted EBOOT.BIN (host intake / tests).
std::vector<std::uint8_t> prepare_from_eboot(std::span<const std::uint8_t> eboot_bin,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress = {});

} // namespace gungnir::platform

// First-run installer: checks the player's disc image, reads its encrypted
// EBOOT.BIN and prepares game.elf from it. Adapted from Yakumo's installer
// (profiles/mhp3rd/host/install/installer.cpp + executable_preparation.cpp),
// which is the PSPRecomp sibling's public reference for the SAF "pick an
// ISO, start the game" flow. Only the single-title identity differs
// (Gungnir USA ULUS10592, tag 0xD91613F0); the ISO9660 + PARAM.SFO +
// ~PSP + AES mechanics are identical.
//
// Identity is pinned without dump hashes (docs/LEGAL.md §5): the caller
// already verified the ISO by size+CRC (redump disc identity), this checks
// DISC_ID + EBOOT header tag, and the prepared output must be an ELF.

#include "platform/android/game_installer.hpp"

#include "platform/android/eboot_prepare.hpp"
#include "save_data/param_sfo.hpp"

// IsoImage lives in third_party/yakumo/src/iso_image.hpp as mhp3rd::IsoImage.
#include "iso_image.hpp"

#include "platform/utf8_path.hpp"
#include "psprecomp/common.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <system_error>

namespace gungnir::platform {
namespace {

constexpr std::size_t kSectorSize = 2048u;

std::vector<std::uint8_t> read_entry(mhp3rd::IsoImage &iso, const mhp3rd::IsoImage::Entry &entry,
    const std::string &what) {
    std::vector<std::uint8_t> data(entry.size);
    if (iso.read(static_cast<std::uint64_t>(entry.lba) * kSectorSize, data) != data.size())
        throw InstallError("The disc image is incomplete: it ends before " + what +
                           ". The file may be truncated or damaged; make the image again.");
    return data;
}

} // namespace

InstallInfo check_image(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        throw InstallError("\"" + mhp3rd::path_to_utf8(path) + "\" is not a file gungnir-recomp can open.");
    {
        std::ifstream in(path, std::ios::binary);
        std::array<char, 4> magic{};
        if (!in.read(magic.data(), magic.size()))
            throw InstallError("\"" + path.filename().string() + "\" is empty or cannot be read.");
        if (std::memcmp(magic.data(), "CISO", 4u) == 0 || std::memcmp(magic.data(), "ZISO", 4u) == 0)
            throw InstallError("\"" + path.filename().string() +
                "\" is a compressed image. Gungnir needs an uncompressed .iso image of the "
                "disc; decompress it first.");
    }

    std::optional<mhp3rd::IsoImage> iso;
    try {
        iso.emplace(path);
    } catch (const std::exception &) {
        throw InstallError("\"" + path.filename().string() +
            "\" is not a disc image gungnir-recomp can read. Choose an uncompressed .iso image "
            "of the game's PSP disc.");
    }

    const auto sfo_entry = iso->find(kGungnirParamSfoPathOnDisc);
    if (!sfo_entry || sfo_entry->directory) {
        if (iso->find("PS3_GAME/PARAM.SFO"))
            throw InstallError("\"" + path.filename().string() + "\" is a PlayStation 3 disc image. "
                "Gungnir needs the PSP disc image of Gungnir: an .iso whose top level holds a "
                "PSP_GAME folder.");
        throw InstallError("\"" + path.filename().string() +
            "\" is not a PSP game disc image: it has no PSP_GAME/PARAM.SFO.");
    }
    const auto sfo_bytes = read_entry(*iso, *sfo_entry, "PSP_GAME/PARAM.SFO");
    const auto sfo = mhp3rd::savedata::ParamSfo::parse(sfo_bytes);
    if (!sfo) throw InstallError("PSP_GAME/PARAM.SFO cannot be read; the image may be damaged.");
    const auto disc_id = sfo->string("DISC_ID");
    if (!disc_id)
        throw InstallError("\"" + path.filename().string() +
            "\" has no disc id in PSP_GAME/PARAM.SFO, so it is not an image gungnir-recomp supports.");
    if (*disc_id != kGungnirDiscId) {
        const auto title = sfo->string("TITLE");
        std::string what = "\"" + path.filename().string() + "\" is ";
        what += title && !title->empty() ? *title + " (" + *disc_id + ")" : "disc " + *disc_id;
        what += ". Gungnir supports only Gungnir, the USA release with disc id ULUS-10592.";
        throw InstallError(what);
    }

    const auto eboot_entry = iso->find(kGungnirExecutablePathOnDisc);
    if (!eboot_entry || eboot_entry->directory)
        throw InstallError("\"" + path.filename().string() +
            "\" has the right disc id but no PSP_GAME/SYSDIR/EBOOT.BIN. The image is "
            "incomplete or modified; make it again from your disc.");
    const auto eboot = read_entry(*iso, *eboot_entry, "PSP_GAME/SYSDIR/EBOOT.BIN");
    if (eboot.size() < 0x150u || std::memcmp(eboot.data(), "~PSP", 4u) != 0)
        throw InstallError("\"" + path.filename().string() +
            "\" has an EBOOT.BIN that is not a PSP executable. The image may be damaged.");
    const std::uint32_t tag = static_cast<std::uint32_t>(eboot[0xD0]) |
        (static_cast<std::uint32_t>(eboot[0xD1]) << 8u) | (static_cast<std::uint32_t>(eboot[0xD2]) << 16u) |
        (static_cast<std::uint32_t>(eboot[0xD3]) << 24u);
    if (tag != 0xD91613F0u)
        throw InstallError("\"" + path.filename().string() +
            "\" is Gungnir (ULUS-10592), but its executable uses an unsupported encryption. "
            "Gungnir supports only the USA release's unmodified executable.");

    return InstallInfo{iso->size_bytes(), *disc_id};
}

std::vector<std::uint8_t> read_eboot(const std::filesystem::path &path) {
    mhp3rd::IsoImage iso(path);
    const auto entry = iso.find(kGungnirExecutablePathOnDisc);
    if (!entry || entry->directory)
        throw InstallError("PSP_GAME/SYSDIR/EBOOT.BIN is missing from the disc image.");
    return read_entry(iso, *entry, "PSP_GAME/SYSDIR/EBOOT.BIN");
}

std::vector<std::uint8_t> prepare_from_eboot(std::span<const std::uint8_t> eboot_bin,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    try {
        return prepare_executable(eboot_bin, progress);
    } catch (const psprecomp::Error &e) {
        throw InstallError(std::string("Preparing the executable failed: ") + e.what() + ".");
    }
}

std::filesystem::path install_from_iso(const std::filesystem::path &iso_path,
    const std::filesystem::path &data_dir,
    const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
    const InstallInfo info = check_image(iso_path);
    (void)info;
    const auto eboot = read_eboot(iso_path);
    if (progress) progress(0u, 1u);
    const auto elf = prepare_from_eboot(eboot, progress);

    std::error_code ec;
    std::filesystem::create_directories(data_dir, ec);
    if (ec) throw InstallError("Setup could not write to \"" + mhp3rd::path_to_utf8(data_dir) + "\": " + ec.message() + ".");
    const std::filesystem::path out = data_dir / "game.elf";
    const std::filesystem::path partial = data_dir / "game.elf.part";
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
        file.close();
        if (!file) {
            std::filesystem::remove(partial, ec);
            throw InstallError("Writing the prepared executable failed. Check that the device has free space.");
        }
    }
    std::filesystem::rename(partial, out, ec);
    if (ec) {
        std::filesystem::remove(partial, ec);
        throw InstallError("Could not keep the prepared executable: " + ec.message() + ".");
    }
    return out;
}

} // namespace gungnir::platform

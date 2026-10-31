#include "hle/hle_utility.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kUtilityInvalidAddress = 0x80110002u;
constexpr std::uint32_t kUtilityInvalidParamSize = 0x80110004u;
constexpr std::uint32_t kUtilityWrongType = 0x80110005u;
constexpr std::uint32_t kUtilityInvalidStatus = 0x80110001u;
constexpr std::uint32_t kSavedataErrorType = 0x80110300u;
constexpr std::uint32_t kSavedataSizesNoData = 0x801103C7u;
constexpr std::uint32_t kSavedataSizesParam = 0x801103C8u;

constexpr std::uint32_t kParamSize = 0x00u;
constexpr std::uint32_t kParamResult = 0x1Cu;
constexpr std::uint32_t kParamMode = 0x30u;
constexpr std::uint32_t kParamGameName = 0x3Cu;
constexpr std::uint32_t kParamSaveName = 0x4Cu;
constexpr std::uint32_t kParamFileName = 0x64u;
constexpr std::uint32_t kParamDataSize = 0x7Cu;
constexpr std::uint32_t kParamIcon0 = 0x584u;
constexpr std::uint32_t kParamIcon1 = 0x594u;
constexpr std::uint32_t kParamPic1 = 0x5A4u;
constexpr std::uint32_t kParamSnd0 = 0x5B4u;
constexpr std::uint32_t kParamMsFree = 0x5D0u;
constexpr std::uint32_t kParamMsData = 0x5D4u;
constexpr std::uint32_t kParamUtilityData = 0x5D8u;

// Gungnir uses the 0x600-byte layout (the only one whose field offsets this
// handler validates). Other versions fail closed instead of being misread.
constexpr std::uint32_t kSavedataSizeV3 = 0x600u;
constexpr std::uint32_t kModeSizes = 8u;
constexpr std::uint32_t kSectorSize = 0x200u;
constexpr std::uint64_t kFreeBytes = 0x40000000ull;

enum class SavedataStatus { None, Init, Running, Finished, Shutdown };

struct SavedataState {
    SavedataStatus status{SavedataStatus::None};
    std::uint32_t param{};
    std::uint32_t mode{};
};

SavedataState g_state;

psprecomp::GuestMemory &memory() {
    return kernel().runtime().memory();
}

std::string read_fixed(psprecomp::GuestMemory &mem, std::uint32_t address, std::uint32_t size) {
    if (address == 0u || !mem.contains(address, size)) return {};
    std::string text;
    for (std::uint32_t i = 0; i < size; ++i) {
        const auto value = static_cast<char>(mem.load8(address + i));
        if (value == '\0') break;
        text.push_back(value);
    }
    return text;
}

void write_fixed(psprecomp::GuestMemory &mem, std::uint32_t address, std::uint32_t size, std::string_view text) {
    if (address == 0u || !mem.contains(address, size)) return;
    for (std::uint32_t i = 0; i < size; ++i)
        mem.store8(address + i, i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0u);
}

bool valid(psprecomp::GuestMemory &mem, std::uint32_t address, std::uint32_t size) {
    return address != 0u && mem.contains(address, size);
}

bool safe_component(std::string_view text) {
    return text != "." && text != ".." && text.find('/') == std::string_view::npos &&
        text.find('\\') == std::string_view::npos;
}

std::uint64_t align_sector(std::uint64_t value) {
    return (value + kSectorSize - 1u) & ~static_cast<std::uint64_t>(kSectorSize - 1u);
}

std::string space_text(std::uint64_t bytes) {
    static constexpr const char *suffixes[] = {"B", "KB", "MB", "GB"};
    std::uint64_t value = bytes;
    for (const char *suffix : suffixes) {
        if (value < 1024u) return std::to_string(value) + " " + suffix;
        value /= 1024u;
    }
    return std::to_string(value) + " TB";
}

struct DirectoryUsage {
    bool exists{};
    std::uint64_t bytes{};
};

DirectoryUsage directory_usage(const std::filesystem::path &path) {
    DirectoryUsage usage;
    std::error_code error;
    usage.exists = std::filesystem::is_directory(path, error);
    if (!usage.exists) return usage;
    for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
        if (error) break;
        const bool regular = entry.is_regular_file(error);
        if (!regular) continue;
        usage.bytes += entry.file_size(error);
    }
    return usage;
}

std::filesystem::path save_directory(std::string_view game, std::string_view save) {
    std::string name(game);
    if (save != "<>") name += save;
    return io_memory_stick_path() / "PSP" / "SAVEDATA" / name;
}

std::uint32_t perform_sizes() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::uint32_t ms_free = mem.load32(param + kParamMsFree);
    const std::uint32_t ms_data = mem.load32(param + kParamMsData);
    const std::uint32_t utility = mem.load32(param + kParamUtilityData);
    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save = read_fixed(mem, param + kParamSaveName, 20u);
    const std::string file = read_fixed(mem, param + kParamFileName, 13u);
    std::uint32_t result = 0u;

    const auto fail = [&](std::uint32_t code) {
        mem.store32(param + kParamResult, code);
        return code;
    };

    if (ms_free != 0u) {
        if (!valid(mem, ms_free, 0x14u)) return fail(kSavedataSizesParam);
        mem.store32(ms_free + 0x00u, kSectorSize);
        mem.store32(ms_free + 0x04u, static_cast<std::uint32_t>(kFreeBytes / kSectorSize));
        mem.store32(ms_free + 0x08u, static_cast<std::uint32_t>(kFreeBytes / 1024u));
        write_fixed(mem, ms_free + 0x0Cu, 8u, space_text(kFreeBytes));
    }
    if (ms_data != 0u) {
        if (!valid(mem, ms_data, 0x40u)) return fail(kSavedataSizesParam);
        const std::string data_game = read_fixed(mem, ms_data + 0x00u, 13u);
        const std::string data_save = read_fixed(mem, ms_data + 0x10u, 20u);
        if (!safe_component(data_game) || !safe_component(data_save)) return fail(kSavedataSizesParam);
        const DirectoryUsage usage = directory_usage(
            save_directory(data_game.empty() ? game : data_game, data_save.empty() ? save : data_save));
        const std::uint32_t clusters =
            static_cast<std::uint32_t>((usage.bytes + kSectorSize - 1u) / kSectorSize);
        mem.store32(ms_data + 0x24u, clusters);
        mem.store32(ms_data + 0x28u, static_cast<std::uint32_t>(usage.bytes / 1024u));
        write_fixed(mem, ms_data + 0x2Cu, 8u, space_text(usage.bytes));
        mem.store32(ms_data + 0x34u, static_cast<std::uint32_t>(usage.bytes / 1024u));
        write_fixed(mem, ms_data + 0x38u, 8u, space_text(usage.bytes));
        if (!usage.exists) result = kSavedataSizesNoData;
    }
    if (utility != 0u) {
        if (!valid(mem, utility, 0x1Cu)) return fail(kSavedataSizesParam);
        std::uint64_t total = kSectorSize + kSectorSize; // directory record + PARAM.SFO
        if (!file.empty()) total += align_sector(mem.load32(param + kParamDataSize));
        total += align_sector(mem.load32(param + kParamIcon0 + 0x08u));
        total += align_sector(mem.load32(param + kParamIcon1 + 0x08u));
        total += align_sector(mem.load32(param + kParamPic1 + 0x08u));
        total += align_sector(mem.load32(param + kParamSnd0 + 0x08u));
        mem.store32(utility + 0x00u, static_cast<std::uint32_t>(total / kSectorSize));
        mem.store32(utility + 0x04u, static_cast<std::uint32_t>(total / 1024u));
        write_fixed(mem, utility + 0x08u, 8u, space_text(total));
        mem.store32(utility + 0x10u, static_cast<std::uint32_t>(total / 1024u));
        write_fixed(mem, utility + 0x14u, 8u, space_text(total));
    }
    mem.store32(param + kParamResult, result);
    return result;
}

std::uint32_t perform_operation() {
    if (g_state.mode != kModeSizes) {
        memory().store32(g_state.param + kParamResult, kSavedataErrorType);
        log_once("savedata-mode:" + std::to_string(g_state.mode),
            "[utility] savedata mode " + std::to_string(g_state.mode) + " is not implemented yet");
        return kSavedataErrorType;
    }
    return perform_sizes();
}

std::uint32_t advance_status() {
    switch (g_state.status) {
    case SavedataStatus::Init:
        g_state.status = SavedataStatus::Running;
        (void)perform_operation();
        return 2u;
    case SavedataStatus::Running:
        g_state.status = SavedataStatus::Finished;
        return 3u;
    case SavedataStatus::Finished:
        return 3u;
    case SavedataStatus::Shutdown:
        g_state.status = SavedataStatus::None;
        return 4u;
    default:
        return kUtilityWrongType;
    }
}

} // namespace

void reset_utility_state() {
    g_state = {};
}

void register_utility(HleRegistrar &hle) {
    hle.add("sceUtility", "sceUtilitySavedataInitStart", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t param = arg(ctx, 0);
        if (g_state.status != SavedataStatus::None) {
            kernel().finish(ctx, kUtilityInvalidStatus);
            return;
        }
        auto &mem = memory();
        if (!valid(mem, param, 4u)) {
            kernel().finish(ctx, kUtilityInvalidAddress);
            return;
        }
        const std::uint32_t size = mem.load32(param + kParamSize);
        if (size != kSavedataSizeV3) {
            kernel().finish(ctx, kUtilityInvalidParamSize);
            return;
        }
        if (!valid(mem, param, size)) {
            kernel().finish(ctx, kUtilityInvalidAddress);
            return;
        }
        const std::string game = read_fixed(mem, param + kParamGameName, 13u);
        const std::string save = read_fixed(mem, param + kParamSaveName, 20u);
        const std::string file = read_fixed(mem, param + kParamFileName, 13u);
        if (!safe_component(game) || !safe_component(save) || !safe_component(file)) {
            kernel().finish(ctx, kUtilityInvalidParamSize);
            return;
        }
        g_state.status = SavedataStatus::Init;
        g_state.param = param;
        g_state.mode = mem.load32(param + kParamMode);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilitySavedataUpdate", [](Runtime &, AllegrexContext &ctx) {
        if (g_state.status == SavedataStatus::None) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        (void)advance_status();
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilitySavedataGetStatus", [](Runtime &, AllegrexContext &ctx) {
        if (g_state.status == SavedataStatus::None) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        kernel().finish(ctx, advance_status());
    });
    hle.add("sceUtility", "sceUtilitySavedataShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        if (g_state.status != SavedataStatus::Finished) {
            kernel().finish(ctx, kUtilityInvalidStatus);
            return;
        }
        g_state.status = SavedataStatus::Shutdown;
        kernel().finish(ctx, 0u);
    });
}

} // namespace gungnir::hle

#include "hle/hle_utility.hpp"

#include "psprecomp/common.hpp"
#include "savedata/save_store.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kUtilityInvalidAddress = 0x80110002u;
constexpr std::uint32_t kUtilityInvalidParamSize = 0x80110004u;
constexpr std::uint32_t kUtilityWrongType = 0x80110005u;
constexpr std::uint32_t kUtilityInvalidStatus = 0x80110001u;
constexpr std::uint32_t kSavedataErrorType = 0x80110300u;
constexpr std::uint32_t kSavedataLoadBroken = 0x80110306u;
constexpr std::uint32_t kSavedataLoadNoData = 0x80110307u;
constexpr std::uint32_t kSavedataLoadParam = 0x80110308u;
constexpr std::uint32_t kSavedataLoadFileMissing = 0x80110309u;
constexpr std::uint32_t kSavedataSaveNoSpace = 0x80110383u;
constexpr std::uint32_t kSavedataSaveParam = 0x80110388u;
constexpr std::uint32_t kSavedataSizesNoData = 0x801103C7u;
constexpr std::uint32_t kSavedataSizesParam = 0x801103C8u;

constexpr std::uint32_t kParamSize = 0x00u;
constexpr std::uint32_t kParamResult = 0x1Cu;
constexpr std::uint32_t kParamMode = 0x30u;
constexpr std::uint32_t kParamBind = 0x34u;
constexpr std::uint32_t kParamGameName = 0x3Cu;
constexpr std::uint32_t kParamSaveName = 0x4Cu;
constexpr std::uint32_t kParamFileName = 0x64u;
constexpr std::uint32_t kParamDataBuf = 0x74u;
constexpr std::uint32_t kParamDataBufSize = 0x78u;
constexpr std::uint32_t kParamDataSize = 0x7Cu;

void trace_savedata(const std::string &text) {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_SAVEDATA") != nullptr;
    static int remaining = 40;
    if (!enabled || remaining <= 0) return;
    --remaining;
    std::fprintf(stderr, "[savedata] %s\n", text.c_str());
}
constexpr std::uint32_t kParamSfoTitle = 0x80u;
constexpr std::uint32_t kParamSfoSavedataTitle = 0x100u;
constexpr std::uint32_t kParamSfoDetail = 0x180u;
constexpr std::uint32_t kParamSfoParental = 0x580u;
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
constexpr std::uint32_t kModeAutoLoad = 0u;
constexpr std::uint32_t kModeAutoSave = 1u;
constexpr std::uint32_t kModeLoad = 2u;
constexpr std::uint32_t kModeSave = 3u;
constexpr std::uint32_t kModeSizes = 8u;
constexpr std::uint32_t kSectorSize = 0x200u;
constexpr std::uint32_t kSavedataBind = 1021u;
constexpr std::uint32_t kFileDataSize = 0x08u;
constexpr std::uint64_t kFreeBytes = 0x40000000ull;

enum class SavedataStatus { None, Init, Running, Finished, Shutdown };

struct SavedataState {
    SavedataStatus status{SavedataStatus::None};
    std::uint32_t param{};
    std::uint32_t mode{};
    bool started{};
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

std::string save_directory_name(std::string_view game, std::string_view save) {
    std::string name(game);
    if (save != "<>") name += save;
    return name;
}

std::filesystem::path save_directory(std::string_view game, std::string_view save) {
    return io_memory_stick_path() / "PSP" / "SAVEDATA" / save_directory_name(game, save);
}

std::vector<std::uint8_t> read_guest(
    psprecomp::GuestMemory &mem, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t i = 0; i < size; ++i) bytes[i] = mem.load8(address + i);
    return bytes;
}

bool copy_guest(
    psprecomp::GuestMemory &mem, std::uint32_t address, std::uint32_t capacity,
    std::span<const std::uint8_t> bytes) {
    if (address == 0u || capacity == 0u || bytes.size() > capacity) return false;
    if (!bytes.empty() && !mem.contains(address, static_cast<std::uint32_t>(bytes.size()))) return false;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        mem.store8(address + static_cast<std::uint32_t>(i), bytes[i]);
    return true;
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

std::uint32_t set_result(std::uint32_t code) {
    memory().store32(g_state.param + kParamResult, code);
    return code;
}

std::uint32_t perform_save() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save_name = read_fixed(mem, param + kParamSaveName, 20u);
    const std::string file = read_fixed(mem, param + kParamFileName, 13u);
    const std::string directory = save_directory_name(game, save_name);
    if (!savedata::SaveStore::valid_name(directory)) return set_result(kSavedataSaveParam);

    savedata::SfoFields fields;
    fields.title = read_fixed(mem, param + kParamSfoTitle, 0x80u);
    fields.savedata_title = read_fixed(mem, param + kParamSfoSavedataTitle, 0x80u);
    fields.detail = read_fixed(mem, param + kParamSfoDetail, 0x400u);
    fields.parental_level = mem.load8(param + kParamSfoParental);

    savedata::SaveDirectory save;
    save.name = directory;
    save.files.push_back({"PARAM.SFO", savedata::build_param_sfo(fields, directory, file)});

    if (!file.empty()) {
        const std::uint32_t buffer = mem.load32(param + kParamDataBuf);
        const std::uint32_t capacity = mem.load32(param + kParamDataBufSize);
        std::uint32_t size = mem.load32(param + kParamDataSize);
        if (size == 0u || size > capacity) size = capacity;
        if (buffer == 0u || capacity == 0u || !mem.contains(buffer, size)) return set_result(kSavedataSaveParam);
        save.files.push_back({file, read_guest(mem, buffer, size)});
    }

    struct Icon {
        std::uint32_t offset;
        const char *name;
    };
    constexpr Icon icons[] = {{kParamIcon0, "ICON0.PNG"}, {kParamIcon1, "ICON1.PNG"},
        {kParamPic1, "PIC1.PNG"}, {kParamSnd0, "SND0.AT3"}};
    for (const Icon &icon : icons) {
        const std::uint32_t buffer = mem.load32(param + icon.offset);
        const std::uint32_t size = mem.load32(param + icon.offset + kFileDataSize);
        if (buffer == 0u || size == 0u) continue;
        if (!mem.contains(buffer, size)) return set_result(kSavedataSaveParam);
        save.files.push_back({icon.name, read_guest(mem, buffer, size)});
    }

    std::string error;
    savedata::SaveStore store(io_memory_stick_path());
    if (!store.write(save, error)) {
        log_once("savedata-save:" + error, "[utility] savedata save failed: " + error);
        return set_result(kSavedataSaveNoSpace);
    }
    return set_result(0u);
}

std::uint32_t perform_load() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    mem.store32(param + kParamDataSize, 0u);

    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save_name = read_fixed(mem, param + kParamSaveName, 20u);
    const std::string file = read_fixed(mem, param + kParamFileName, 13u);
    const std::string directory = save_directory_name(game, save_name);
    if (!savedata::SaveStore::valid_name(directory)) return set_result(kSavedataLoadParam);

    savedata::SaveStore store(io_memory_stick_path());
    if (!store.read(directory)) return set_result(kSavedataLoadNoData);
    const auto sfo_bytes = store.read_file(directory, "PARAM.SFO");
    if (!sfo_bytes) return set_result(kSavedataLoadBroken);
    const auto fields = savedata::parse_sfo_fields(*sfo_bytes);
    if (!fields) return set_result(kSavedataLoadBroken);

    write_fixed(mem, param + kParamSfoTitle, 0x80u, fields->title.substr(0u, 0x7Fu));
    write_fixed(mem, param + kParamSfoSavedataTitle, 0x80u, fields->savedata_title.substr(0u, 0x7Fu));
    write_fixed(mem, param + kParamSfoDetail, 0x400u, fields->detail.substr(0u, 0x3FFu));
    mem.store8(param + kParamSfoParental, fields->parental_level);
    mem.store32(param + kParamBind, kSavedataBind);

    if (!file.empty()) {
        const auto bytes = store.read_file(directory, file);
        if (!bytes) return set_result(kSavedataLoadFileMissing);
        const std::uint32_t buffer = mem.load32(param + kParamDataBuf);
        const std::uint32_t capacity = mem.load32(param + kParamDataBufSize);
        if (!copy_guest(mem, buffer, capacity, *bytes)) return set_result(kSavedataLoadParam);
        mem.store32(param + kParamDataSize, static_cast<std::uint32_t>(bytes->size()));
    }

    struct Icon {
        std::uint32_t offset;
        const char *name;
    };
    constexpr Icon icons[] = {{kParamIcon0, "ICON0.PNG"}, {kParamIcon1, "ICON1.PNG"},
        {kParamPic1, "PIC1.PNG"}, {kParamSnd0, "SND0.AT3"}};
    for (const Icon &icon : icons) {
        const std::uint32_t buffer = mem.load32(param + icon.offset);
        const std::uint32_t capacity = mem.load32(param + icon.offset + 0x04u);
        if (buffer == 0u || capacity == 0u) continue;
        const auto bytes = store.read_file(directory, icon.name);
        std::uint32_t copied = 0u;
        if (bytes) {
            const auto count = std::min<std::size_t>(bytes->size(), capacity);
            if (!copy_guest(mem, buffer, capacity, std::span<const std::uint8_t>(*bytes).first(count)))
                return set_result(kSavedataLoadParam);
            copied = static_cast<std::uint32_t>(count);
        }
        mem.store32(param + icon.offset + kFileDataSize, copied);
    }
    return set_result(0u);
}

std::uint32_t perform_operation() {
    switch (g_state.mode) {
    case kModeAutoLoad:
        return perform_load();
    case kModeAutoSave:
        return perform_save();
    case kModeSizes:
        return perform_sizes();
    case kModeSave:
        return perform_save();
    case kModeLoad:
        return perform_load();
    default:
        memory().store32(g_state.param + kParamResult, kSavedataErrorType);
        log_once("savedata-mode:" + std::to_string(g_state.mode),
            "[utility] savedata mode " + std::to_string(g_state.mode) + " is not implemented yet");
        return kSavedataErrorType;
    }
}

std::uint32_t advance_status_inner();

std::uint32_t advance_status() {
    const SavedataStatus before = g_state.status;
    const std::uint32_t result = advance_status_inner();
    trace_savedata("status " + std::to_string(static_cast<int>(before)) + " -> " +
                   std::to_string(static_cast<int>(g_state.status)) + " returned " +
                   std::to_string(result));
    return result;
}

std::uint32_t advance_status_inner() {
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
    case SavedataStatus::None:
        return 0u;
    }
    return 0u;
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
        g_state.started = true;
        trace_savedata("init mode=" + std::to_string(g_state.mode) + " game=" + game + " save=" + save +
                       " file=" + file);
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilitySavedataUpdate", [](Runtime &, AllegrexContext &ctx) {
        if (!g_state.started) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        if (g_state.status != SavedataStatus::None) (void)advance_status();
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilitySavedataGetStatus", [](Runtime &, AllegrexContext &ctx) {
        if (!g_state.started) {
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

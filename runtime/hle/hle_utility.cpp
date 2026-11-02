#include "hle/hle_utility.hpp"

#include "psprecomp/common.hpp"
#include "savedata/save_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
constexpr std::uint32_t kSavedataListParam = 0x80110328u;

constexpr std::uint32_t kParamSize = 0x00u;
constexpr std::uint32_t kParamResult = 0x1Cu;
constexpr std::uint32_t kParamMode = 0x30u;
constexpr std::uint32_t kParamBind = 0x34u;
constexpr std::uint32_t kParamGameName = 0x3Cu;
constexpr std::uint32_t kParamSaveName = 0x4Cu;
constexpr std::uint32_t kParamSaveNameList = 0x60u;
constexpr std::uint32_t kParamFileName = 0x64u;
constexpr std::uint32_t kParamDataBuf = 0x74u;
constexpr std::uint32_t kParamDataBufSize = 0x78u;
constexpr std::uint32_t kParamDataSize = 0x7Cu;

void trace_savedata(const std::string &text) {
    static const bool enabled = std::getenv("GUNGNIR_TRACE_SAVEDATA") != nullptr;
    static std::uint64_t remaining = [] {
        const char *limit = std::getenv("GUNGNIR_TRACE_SAVEDATA_LIMIT");
        return limit != nullptr ? std::strtoull(limit, nullptr, 10) : 40ull;
    }();
    if (!enabled || remaining == 0u) return;
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
constexpr std::uint32_t kParamFocus = 0x5C8u;
constexpr std::uint32_t kParamIdList = 0x5F4u;

// SceUtilitySavedataIdListInfo: maxCount, resultCount, entries.
constexpr std::uint32_t kIdListMaxCount = 0x00u;
constexpr std::uint32_t kIdListResultCount = 0x04u;
constexpr std::uint32_t kIdListEntries = 0x08u;
// SceUtilitySavedataIdListEntry: st_mode, st_ctime, st_atime, st_mtime, name[20].
constexpr std::uint32_t kListEntrySize = 0x48u;
constexpr std::uint32_t kListEntryMode = 0x00u;
constexpr std::uint32_t kListEntryCTime = 0x04u;
constexpr std::uint32_t kListEntryATime = 0x14u;
constexpr std::uint32_t kListEntryMTime = 0x24u;
constexpr std::uint32_t kListEntryName = 0x34u;
constexpr std::uint32_t kListEntryNameSize = 20u;
constexpr std::uint32_t kListEntryFileMode = 0x11FFu;

constexpr std::uint32_t kMsgDialogCommonResult = 0x1Cu;
constexpr std::uint32_t kMsgDialogResult = 0x30u;
constexpr std::uint32_t kMsgDialogOptions = 0x23Cu;
constexpr std::uint32_t kMsgDialogButtonPressed = 0x240u;
constexpr std::uint32_t kMsgDialogSizeV1 = 572u;
constexpr std::uint32_t kMsgDialogSizeV2 = 580u;
constexpr std::uint32_t kMsgDialogSizeV3 = 708u;
constexpr std::uint32_t kMsgDialogPressedYes = 1u;

// Gungnir uses the 0x600-byte layout (the only one whose field offsets this
// handler validates). Other versions fail closed instead of being misread.
constexpr std::uint32_t kSavedataSizeV3 = 0x600u;
constexpr std::uint32_t kModeAutoLoad = 0u;
constexpr std::uint32_t kModeAutoSave = 1u;
constexpr std::uint32_t kModeLoad = 2u;
constexpr std::uint32_t kModeSave = 3u;
constexpr std::uint32_t kModeListLoad = 4u;
constexpr std::uint32_t kModeListSave = 5u;
constexpr std::uint32_t kModeSizes = 8u;
constexpr std::uint32_t kModeList = 11u;
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

// The message dialog has no UI here. It runs the same status machine and
// auto-accepts with YES once the dialog becomes visible, which is what a
// game observes when a player presses the default button. The accepted
// result is written to the parameter block so the caller can read it back.
struct MsgDialogState {
    SavedataStatus status{SavedataStatus::None};
    std::uint32_t param{};
    std::uint32_t size{};
    bool started{};
};

SavedataState g_state;
MsgDialogState g_dialog;

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

std::filesystem::path savedata_root() {
    return io_memory_stick_path() / "PSP" / "SAVEDATA";
}

std::filesystem::path save_directory(std::string_view game, std::string_view save) {
    return savedata_root() / save_directory_name(game, save);
}

bool wildcard_match(std::string_view text, std::string_view pattern) {
    std::size_t t = 0u;
    std::size_t p = 0u;
    std::size_t star = std::string_view::npos;
    std::size_t star_text = 0u;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++t;
            ++p;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            star_text = t;
        } else if (star != std::string_view::npos) {
            p = star + 1u;
            t = ++star_text;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::time_t file_time(const std::filesystem::path &path) {
    std::error_code error;
    const auto stamp = std::filesystem::last_write_time(path, error);
    if (error) return std::time(nullptr);
    const auto system = std::chrono::file_clock::to_sys(stamp);
    return std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(system));
}

// ScePspDateTime: year, month, day, hour, minute, second, microsecond.
void store_psp_time(psprecomp::GuestMemory &mem, std::uint32_t address, std::time_t time) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    mem.store16(address + 0x00u, static_cast<std::uint16_t>(tm.tm_year + 1900));
    mem.store16(address + 0x02u, static_cast<std::uint16_t>(tm.tm_mon + 1));
    mem.store16(address + 0x04u, static_cast<std::uint16_t>(tm.tm_mday));
    mem.store16(address + 0x06u, static_cast<std::uint16_t>(tm.tm_hour));
    mem.store16(address + 0x08u, static_cast<std::uint16_t>(tm.tm_min));
    mem.store16(address + 0x0Au, static_cast<std::uint16_t>(tm.tm_sec));
    mem.store32(address + 0x0Cu, 0u);
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

std::uint32_t perform_save(std::string_view save_override = {}) {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save_name = save_override.empty()
        ? read_fixed(mem, param + kParamSaveName, 20u)
        : std::string(save_override);
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
    // A list dialog writes the selected save name back into the request.
    if (!save_override.empty() && !file.empty())
        write_fixed(mem, param + kParamSaveName, 20u, save_name);
    return set_result(0u);
}

std::uint32_t perform_load(std::string_view save_override = {}) {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    mem.store32(param + kParamDataSize, 0u);

    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save_name = save_override.empty()
        ? read_fixed(mem, param + kParamSaveName, 20u)
        : std::string(save_override);
    const std::string file = read_fixed(mem, param + kParamFileName, 13u);
    const std::string directory = save_directory_name(game, save_name);
    if (!savedata::SaveStore::valid_name(directory)) return set_result(kSavedataLoadParam);

    savedata::SaveStore store(io_memory_stick_path());
    if (!store.read(directory)) return set_result(kSavedataLoadNoData);
    // A list dialog writes the selected save name back into the request.
    if (!save_override.empty() && !file.empty())
        write_fixed(mem, param + kParamSaveName, 20u, save_name);
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

std::uint32_t perform_list() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::uint32_t id_list = mem.load32(param + kParamIdList);
    if (!valid(mem, id_list, kIdListEntries + 0x04u)) return set_result(kSavedataListParam);
    const std::uint32_t max_count = mem.load32(id_list + kIdListMaxCount);
    const std::uint32_t entries = mem.load32(id_list + kIdListEntries);

    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save = read_fixed(mem, param + kParamSaveName, 20u);
    if (!safe_component(game) || !safe_component(save)) return set_result(kSavedataListParam);
    const std::string pattern = game + (save == "<>" ? std::string() : save);

    std::uint32_t count = 0u;
    if (max_count != 0u) {
        if (static_cast<std::uint64_t>(max_count) * kListEntrySize > 0xFFFFFFFFull ||
            !valid(mem, entries, max_count * kListEntrySize))
            return set_result(kSavedataListParam);
        savedata::SaveStore store(io_memory_stick_path());
        for (const std::string &name : store.list(game)) {
            if (!wildcard_match(name, pattern)) continue;
            if (count >= max_count) break;
            const std::string save_name = name.substr(game.size());
            const std::uint32_t entry = entries + count * kListEntrySize;
            for (std::uint32_t i = 0; i < kListEntrySize; ++i) mem.store8(entry + i, 0u);
            mem.store32(entry + kListEntryMode, kListEntryFileMode);
            const std::filesystem::path directory = savedata_root() / name;
            const std::filesystem::path sfo = directory / "PARAM.SFO";
            std::error_code error;
            const std::time_t time = file_time(std::filesystem::is_regular_file(sfo, error) ? sfo : directory);
            store_psp_time(mem, entry + kListEntryCTime, time);
            store_psp_time(mem, entry + kListEntryATime, time);
            store_psp_time(mem, entry + kListEntryMTime, time);
            write_fixed(mem, entry + kListEntryName, kListEntryNameSize,
                save_name.substr(0u, kListEntryNameSize - 1u));
            ++count;
        }
    }
    mem.store32(id_list + kIdListResultCount, count);
    trace_savedata("list pattern=" + pattern + " count=" + std::to_string(count) +
                   " max=" + std::to_string(max_count));
    return set_result(0u);
}

struct ListCandidate {
    std::string name;
    std::uint64_t bytes{};
    std::time_t time{};
};

// SceUtilitySavedataParam::saveNameList is a zero-terminated array of 20-byte
// names. An absent or empty list falls back to the single saveName, and "<>"
// means "no particular save" rather than a literal name.
std::vector<std::string> read_save_name_list(std::string save) {
    auto &mem = memory();
    std::vector<std::string> names;
    const std::uint32_t address = mem.load32(g_state.param + kParamSaveNameList);
    if (valid(mem, address, 20u)) {
        for (std::uint32_t i = 0u; i < 128u; ++i) {
            if (!valid(mem, address + i * 20u, 20u)) break;
            std::string entry = read_fixed(mem, address + i * 20u, 20u);
            if (entry.empty()) break;
            names.push_back(std::move(entry));
        }
    }
    if (names.empty() && !save.empty() && save != "<>") names.push_back(std::move(save));
    return names;
}

std::vector<ListCandidate> listload_candidates(
    const std::string &game, const std::vector<std::string> &names) {
    std::vector<ListCandidate> candidates;
    const auto add = [&](std::string name) {
        if (!savedata::SaveStore::valid_name(name)) return;
        for (const ListCandidate &candidate : candidates)
            if (candidate.name == name) return;
        const std::filesystem::path directory = save_directory(game, name);
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error)) return;
        const std::filesystem::path sfo = directory / "PARAM.SFO";
        ListCandidate candidate;
        candidate.name = std::move(name);
        candidate.bytes = directory_usage(directory).bytes;
        candidate.time = file_time(std::filesystem::is_regular_file(sfo, error) ? sfo : directory);
        candidates.push_back(std::move(candidate));
    };
    for (const std::string &name : names) {
        if (name != "<>") {
            add(name);
            continue;
        }
        savedata::SaveStore store(io_memory_stick_path());
        for (const std::string &directory : store.list(game))
            add(directory.substr(game.size()));
    }
    return candidates;
}

// PSP focus values: unknown/first-list/first-data default to the first entry.
std::size_t select_candidate(const std::vector<ListCandidate> &candidates, std::uint32_t focus) {
    std::size_t selected = 0u;
    switch (focus) {
    case 2u:
        return candidates.size() - 1u;
    case 3u:
        for (std::size_t i = 1u; i < candidates.size(); ++i)
            if (candidates[i].time > candidates[selected].time) selected = i;
        return selected;
    case 4u:
        for (std::size_t i = 1u; i < candidates.size(); ++i)
            if (candidates[i].time < candidates[selected].time) selected = i;
        return selected;
    case 5u:
        for (std::size_t i = 0u; i < candidates.size(); ++i)
            if (candidates[i].bytes != 0u) return i;
        return 0u;
    case 6u:
        for (std::size_t i = candidates.size(); i > 0u;) {
            --i;
            if (candidates[i].bytes != 0u) return i;
        }
        return 0u;
    case 7u:
        for (std::size_t i = 0u; i < candidates.size(); ++i)
            if (candidates[i].bytes == 0u) return i;
        return 0u;
    case 8u:
        for (std::size_t i = candidates.size(); i > 0u;) {
            --i;
            if (candidates[i].bytes == 0u) return i;
        }
        return 0u;
    default:
        return 0u;
    }
}

// There is no save-picker UI. An empty list auto-cancels with NO_DATA; a
// non-empty list auto-selects the caller's focus entry and loads it, with the
// selected name written back into the request.
std::uint32_t perform_list_load() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save = read_fixed(mem, param + kParamSaveName, 20u);
    if (!safe_component(game) || !safe_component(save)) return set_result(kSavedataLoadParam);
    const std::vector<ListCandidate> candidates = listload_candidates(game, read_save_name_list(save));
    if (candidates.empty()) {
        trace_savedata("listload no data game=" + game + " save=" + save);
        return set_result(kSavedataLoadNoData);
    }
    const std::size_t selected = select_candidate(candidates, mem.load32(param + kParamFocus));
    trace_savedata("listload candidates=" + std::to_string(candidates.size()) +
                   " selected=" + candidates[selected].name);
    return perform_load(candidates[selected].name);
}

// There is no save-picker UI. The headless list-save honors the caller's
// focus entry of saveNameList (PPSSPP's initial currentSelectedSave) and
// writes it, with the selected name written back into the request. Focus
// values and fallbacks mirror PPSSPP's SavedataParam (FIRSTLIST=first,
// LASTLIST=last, LATEST/OLDEST by mtime, FIRST/LASTDATA/EMPTY, NAME=saveName
// index; everything unknown or unsatisfiable falls back to the first entry).
std::uint32_t perform_list_save() {
    auto &mem = memory();
    const std::uint32_t param = g_state.param;
    const std::string game = read_fixed(mem, param + kParamGameName, 13u);
    const std::string save = read_fixed(mem, param + kParamSaveName, 20u);
    if (!safe_component(game) || !safe_component(save)) return set_result(kSavedataSaveParam);
    const std::vector<std::string> names = read_save_name_list(save);
    if (names.empty()) return set_result(kSavedataSaveParam);
    const std::vector<ListCandidate> candidates = listload_candidates(game, names);
    std::vector<std::time_t> modified(names.size(), 0);
    std::vector<bool> present(names.size(), false);
    for (const ListCandidate &candidate : candidates) {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] != candidate.name) continue;
            present[i] = true;
            modified[i] = candidate.time;
        }
    }
    const std::uint32_t focus = mem.load32(param + kParamFocus);
    std::size_t selected = 0u;
    switch (focus) {
    case 0u: {
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (names[i] != save) continue;
            selected = i;
            break;
        }
        break;
    }
    case 1u:
        break;
    case 2u:
        selected = names.size() - 1u;
        break;
    case 3u:
        for (std::size_t i = 0; i < names.size(); ++i)
            if (present[i] && (!present[selected] || modified[i] > modified[selected])) selected = i;
        break;
    case 4u:
        for (std::size_t i = 0; i < names.size(); ++i)
            if (present[i] && (!present[selected] || modified[i] < modified[selected])) selected = i;
        break;
    case 5u:
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (!present[i]) continue;
            selected = i;
            break;
        }
        break;
    case 6u:
        for (std::size_t i = names.size(); i > 0u;) {
            --i;
            if (!present[i]) continue;
            selected = i;
            break;
        }
        break;
    case 7u:
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (present[i]) continue;
            selected = i;
            break;
        }
        break;
    case 8u:
        for (std::size_t i = names.size(); i > 0u;) {
            --i;
            if (present[i]) continue;
            selected = i;
            break;
        }
        break;
    default:
        break;
    }
    trace_savedata("listsave names=" + std::to_string(names.size()) +
                   " selected=" + names[selected]);
    return perform_save(names[selected]);
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
    case kModeListLoad:
        return perform_list_load();
    case kModeListSave:
        return perform_list_save();
    case kModeLoad:
        return perform_load();
    case kModeList:
        return perform_list();
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

// The dialog has no renderer. Accepting on the first visible frame and
// writing result 0 plus buttonPressed = YES is the headless equivalent of the
// player pressing the default button; it follows the message-dialog state
// machine used by PPSSPP (Core/Dialog/PSPMsgDialog.cpp).
void accept_message_dialog() {
    auto &mem = memory();
    mem.store32(g_dialog.param + kMsgDialogCommonResult, 0u);
    mem.store32(g_dialog.param + kMsgDialogResult, 0u);
    if (g_dialog.size >= kMsgDialogSizeV2)
        mem.store32(g_dialog.param + kMsgDialogButtonPressed, kMsgDialogPressedYes);
}

std::uint32_t advance_dialog_status() {
    switch (g_dialog.status) {
    case SavedataStatus::Init:
        g_dialog.status = SavedataStatus::Running;
        return 2u;
    case SavedataStatus::Running:
        accept_message_dialog();
        g_dialog.status = SavedataStatus::Finished;
        return 3u;
    case SavedataStatus::Finished:
        return 3u;
    case SavedataStatus::Shutdown:
        g_dialog.status = SavedataStatus::None;
        return 4u;
    case SavedataStatus::None:
        return 0u;
    }
    return 0u;
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
    g_dialog = {};
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
        std::string names;
        const std::uint32_t name_list = mem.load32(param + 0x60u);
        for (std::uint32_t i = 0u; i < 32u; ++i) {
            if (!valid(mem, name_list + i * 20u, 20u)) break;
            const std::string entry = read_fixed(mem, name_list + i * 20u, 20u);
            if (entry.empty()) break;
            names += (names.empty() ? "[\"" : ",\"") + entry + "\"";
        }
        if (!names.empty()) names += "]";
        trace_savedata("init mode=" + std::to_string(g_state.mode) + " game=" + game + " save=" + save +
                       " file=" + file + " focus=" + std::to_string(mem.load32(param + 0x5C8u)) +
                       " saveNameList=" + psprecomp::hex32(name_list) + names + " idList=" +
                       psprecomp::hex32(mem.load32(param + kParamIdList)));
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
    hle.add("sceUtility", "sceUtilityMsgDialogInitStart", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t param = arg(ctx, 0);
        if (g_dialog.status != SavedataStatus::None) {
            kernel().finish(ctx, kUtilityInvalidStatus);
            return;
        }
        auto &mem = memory();
        if (!valid(mem, param, 4u)) {
            kernel().finish(ctx, kUtilityInvalidAddress);
            return;
        }
        const std::uint32_t size = mem.load32(param + kParamSize);
        if (size != kMsgDialogSizeV1 && size != kMsgDialogSizeV2 && size != kMsgDialogSizeV3) {
            kernel().finish(ctx, kUtilityInvalidParamSize);
            return;
        }
        if (!valid(mem, param, size)) {
            kernel().finish(ctx, kUtilityInvalidAddress);
            return;
        }
        g_dialog.status = SavedataStatus::Init;
        g_dialog.param = param;
        g_dialog.size = size;
        g_dialog.started = true;
        trace_savedata("msgdialog init size=" + std::to_string(size) +
                       " mode=" + std::to_string(mem.load32(param + 0x34u)) +
                       " options=" + std::to_string(mem.load32(param + kMsgDialogOptions)));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityMsgDialogUpdate", [](Runtime &, AllegrexContext &ctx) {
        if (!g_dialog.started) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        if (g_dialog.status != SavedataStatus::None) (void)advance_dialog_status();
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityMsgDialogGetStatus", [](Runtime &, AllegrexContext &ctx) {
        if (!g_dialog.started) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        kernel().finish(ctx, advance_dialog_status());
    });
    hle.add("sceUtility", "sceUtilityMsgDialogShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        if (!g_dialog.started) {
            kernel().finish(ctx, kUtilityWrongType);
            return;
        }
        if (g_dialog.status != SavedataStatus::Finished) {
            kernel().finish(ctx, kUtilityInvalidStatus);
            return;
        }
        g_dialog.status = SavedataStatus::Shutdown;
        kernel().finish(ctx, 0u);
    });
}

namespace {

void uput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

struct UReader {
    const std::uint8_t *p{};
    std::size_t left{};
    bool ok{true};

    [[nodiscard]] std::uint32_t u32() {
        if (left < 4u) {
            ok = false;
            return 0u;
        }
        const std::uint32_t v = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
            (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
        p += 4;
        left -= 4;
        return v;
    }

    [[nodiscard]] std::uint8_t u8() {
        if (left < 1u) {
            ok = false;
            return 0u;
        }
        const std::uint8_t v = *p;
        ++p;
        --left;
        return v;
    }
};

} // namespace

bool export_snapshot_utility(std::vector<std::uint8_t> &bytes_out, std::string &error) {
    bytes_out.clear();
    uput_u32(bytes_out, static_cast<std::uint32_t>(g_state.status));
    uput_u32(bytes_out, g_state.param);
    uput_u32(bytes_out, g_state.mode);
    bytes_out.push_back(g_state.started ? 1u : 0u);
    uput_u32(bytes_out, static_cast<std::uint32_t>(g_dialog.status));
    uput_u32(bytes_out, g_dialog.param);
    uput_u32(bytes_out, g_dialog.size);
    bytes_out.push_back(g_dialog.started ? 1u : 0u);
    error.clear();
    return true;
}

bool import_snapshot_utility(std::span<const std::uint8_t> bytes, std::string &error) {
    UReader in{bytes.data(), bytes.size()};
    const std::uint32_t state_status = in.u32();
    const std::uint32_t state_param = in.u32();
    const std::uint32_t state_mode = in.u32();
    const std::uint8_t state_started = in.u8();
    const std::uint32_t dialog_status = in.u32();
    const std::uint32_t dialog_param = in.u32();
    const std::uint32_t dialog_size = in.u32();
    const std::uint8_t dialog_started = in.u8();
    if (!in.ok || in.left != 0u || state_status > 4u || dialog_status > 4u || state_started > 1u ||
        dialog_started > 1u) {
        error = "snapshot_bad_length: utility section malformed";
        return false;
    }
    g_state.status = static_cast<SavedataStatus>(state_status);
    g_state.param = state_param;
    g_state.mode = state_mode;
    g_state.started = state_started != 0u;
    g_dialog.status = static_cast<SavedataStatus>(dialog_status);
    g_dialog.param = dialog_param;
    g_dialog.size = dialog_size;
    g_dialog.started = dialog_started != 0u;
    error.clear();
    return true;
}

} // namespace gungnir::hle

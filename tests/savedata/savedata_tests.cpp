#include "savedata/save_store.hpp"
#include "save_data/param_sfo.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using gungnir::savedata::SaveDirectory;
using gungnir::savedata::SaveFile;
using gungnir::savedata::SaveStore;
using gungnir::savedata::SfoFields;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (condition) return;
    std::cerr << "savedata_tests: FAIL: " << what << "\n";
    ++failures;
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

void copy_tree(const fs::path &from, const fs::path &to) {
    std::error_code error;
    fs::create_directories(to.parent_path(), error);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    check(!error, "fixture copy succeeds");
}

void synthetic_contracts(const fs::path &scratch) {
    SfoFields fields;
    fields.title = "Fixture Title";
    fields.savedata_title = "Fixture Save";
    fields.detail = "Synthetic savedata fixture";
    fields.parental_level = 3;

    const auto sfo = gungnir::savedata::build_param_sfo(fields, "ULUS10592FIXTURE", "DATA.BIN");
    check(!sfo.empty(), "synthetic PARAM.SFO is produced");
    const auto parsed = mhp3rd::savedata::ParamSfo::parse(sfo);
    check(parsed.has_value(), "synthetic PARAM.SFO parses");
    if (parsed) {
        check(parsed->serialize() == sfo, "synthetic PARAM.SFO round-trips byte-identically");
        check(parsed->string("SAVEDATA_DIRECTORY").value_or("") == "ULUS10592FIXTURE",
            "synthetic SFO records the save directory");
        check(parsed->string("CATEGORY").value_or("") == "MS", "synthetic SFO uses category MS");
        check(parsed->integer("PARENTAL_LEVEL").value_or(0u) == 3u, "synthetic SFO records parental level");
        const auto *list = parsed->binary("SAVEDATA_FILE_LIST");
        check(list != nullptr && list->size() == 99u * 32u, "synthetic SFO has a full file list");
        if (list != nullptr && list->size() >= 13u)
            check(std::string(reinterpret_cast<const char *>(list->data()), 8u) == "DATA.BIN",
                "synthetic file list names the data file");
    }
    const auto fields_back = gungnir::savedata::parse_sfo_fields(sfo);
    check(fields_back.has_value(), "synthetic SFO exposes its fields");
    if (fields_back) {
        check(fields_back->title == fields.title, "title survives the round-trip");
        check(fields_back->savedata_title == fields.savedata_title, "savedata title survives the round-trip");
        check(fields_back->detail == fields.detail, "detail survives the round-trip");
        check(fields_back->parental_level == fields.parental_level, "parental level survives the round-trip");
    }

    SaveStore store(scratch);
    SaveDirectory save;
    save.name = "ULUS10592FIXTURE";
    save.files = {{"DATA.BIN", bytes_of("synthetic-save-payload")}, {"PARAM.SFO", sfo}};
    std::string error;
    check(store.write(save, error), "synthetic save writes");
    check(error.empty(), "synthetic save reports no error");

    const auto read = store.read(save.name);
    check(read.has_value(), "synthetic save reads back");
    if (read) {
        check(read->files.size() == 2u, "synthetic save keeps both files");
        for (const SaveFile &file : read->files) {
            const auto expected = std::find_if(save.files.begin(), save.files.end(),
                [&](const SaveFile &candidate) { return candidate.name == file.name; });
            check(expected != save.files.end(), "synthetic save has no extra file");
            if (expected != save.files.end()) check(file.bytes == expected->bytes, "synthetic file bytes survive");
        }
    }

    const auto listed = store.list("ULUS10592");
    check(listed.size() == 1u && listed.front() == save.name, "synthetic save is listed by prefix");
    check(store.list("OTHER").empty(), "unrelated prefix lists nothing");

    const fs::path second_root = scratch / "second";
    SaveStore second(second_root);
    check(second.write(*store.read(save.name), error), "read save writes to a second root");
    const auto second_read = second.read(save.name);
    check(second_read.has_value(), "second root reads the copied save");
    if (read && second_read) {
        check(read->files.size() == second_read->files.size(), "copied save keeps its file count");
        for (std::size_t i = 0; i < read->files.size() && i < second_read->files.size(); ++i) {
            check(read->files[i].name == second_read->files[i].name, "copied save keeps file order");
            check(read->files[i].bytes == second_read->files[i].bytes, "copied save stays byte-identical");
        }
    }

    SaveDirectory traversal;
    traversal.name = "..";
    traversal.files = {{"DATA.BIN", {1u}}};
    check(!store.write(traversal, error), "directory traversal is rejected");
    check(store.read("../ULUS10592FIXTURE") == std::nullopt, "traversal read returns nothing");
    const std::uint8_t one_byte[] = {1u};
    check(!store.write_file(save.name, "../escape", one_byte, error), "file traversal is rejected");
    check(!store.write_file(save.name, "", one_byte, error), "empty file name is rejected");
    check(!SaveStore::valid_name(".") && !SaveStore::valid_name(".."), "dot names are invalid");
    check(!SaveStore::valid_name("a/b") && !SaveStore::valid_name("a\\b"), "separators are invalid");
    check(SaveStore::valid_name("ULUS10592FIXTURE"), "a plain save name is valid");
}

void fixture_contracts(const fs::path &fixtures, const fs::path &scratch) {
    std::vector<fs::path> saves;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(fixtures, error)) {
        if (error) break;
        if (!entry.is_directory(error)) continue;
        const std::string name = entry.path().filename().string();
        if (name.rfind("ULUS10592", 0) == 0) saves.push_back(entry.path());
    }
    check(!saves.empty(), "fixture directory contains a save");

    const fs::path imported_root = scratch / "imported";
    const fs::path rewritten_root = scratch / "rewritten";
    SaveStore imported(imported_root);
    SaveStore rewritten(rewritten_root);
    std::string message;

    for (const fs::path &source : saves) {
        const fs::path target = imported_root / "PSP" / "SAVEDATA" / source.filename();
        copy_tree(source, target);
        const auto save = imported.read(source.filename().string());
        check(save.has_value(), "exported save reads through the store");
        if (!save) continue;
        check(!save->files.empty(), "exported save has files");
        for (const SaveFile &file : save->files) {
            if (file.name != "PARAM.SFO") continue;
            const auto sfo = mhp3rd::savedata::ParamSfo::parse(file.bytes);
            check(sfo.has_value(), "exported PARAM.SFO parses");
            if (sfo) check(sfo->serialize() == file.bytes, "exported PARAM.SFO round-trips byte-identically");
            check(gungnir::savedata::parse_sfo_fields(file.bytes).has_value(),
                "exported PARAM.SFO exposes its fields");
        }
        check(rewritten.write(*save, message), "exported save rewrites");
        const auto copy = rewritten.read(save->name);
        check(copy.has_value(), "rewritten save reads back");
        if (!copy) continue;
        check(copy->files.size() == save->files.size(), "rewritten save keeps its file count");
        for (std::size_t i = 0; i < save->files.size() && i < copy->files.size(); ++i) {
            check(save->files[i].name == copy->files[i].name, "rewritten save keeps file order");
            check(save->files[i].bytes == copy->files[i].bytes, "rewritten save stays byte-identical");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    fs::path fixtures;
    fs::path scratch;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--fixtures" && i + 1 < argc) {
            fixtures = argv[++i];
        } else if (arg == "--scratch" && i + 1 < argc) {
            scratch = argv[++i];
        } else {
            std::cerr << "savedata_tests: unknown argument " << arg << "\n";
            return 2;
        }
    }
    if (scratch.empty()) {
        std::cerr << "savedata_tests: --scratch is required\n";
        return 2;
    }

    synthetic_contracts(scratch / "synthetic");
    if (!fixtures.empty()) fixture_contracts(fixtures, scratch / "fixtures");

    if (failures != 0) {
        std::cerr << "savedata_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "savedata_tests: OK\n";
    return 0;
}

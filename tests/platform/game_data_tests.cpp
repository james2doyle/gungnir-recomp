// T9c contract tests: game-data discovery, identity gate and asset stamps.
// Every fixture is hand-authored bytes; no game data is read.

#include "platform/android/assets.hpp"
#include "platform/android/game_data.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

namespace {

namespace fs = std::filesystem;
using namespace gungnir::platform;

void check(bool condition, const char *contract) {
    if (!condition) throw std::runtime_error(contract);
}

fs::path make_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path =
        fs::temp_directory_path() / ("gungnir_game_data_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    fs::create_directories(path);
    return path;
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<std::uint8_t>((i * 37u + seed) & 0xFFu);
    return bytes;
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

std::uint32_t crc_of(const std::vector<std::uint8_t> &bytes) {
    uLong crc = crc32(0L, Z_NULL, 0);
    return static_cast<std::uint32_t>(
        crc32(crc, bytes.data(), static_cast<uInt>(bytes.size())));
}

GameIdentity identity_of(const std::vector<std::uint8_t> &bytes) {
    return {bytes.size(), crc_of(bytes)};
}

void identity_gate() {
    const fs::path directory = make_temp_dir();
    const std::vector<std::uint8_t> bytes = pattern(64u * 1024u, 7);
    const fs::path iso = directory / "game.iso";
    write_file(iso, bytes);
    const GameIdentity identity = identity_of(bytes);

    const GameDataResult accepted = verify_game_data(iso, identity);
    check(accepted.ok && accepted.error.empty(), "identity accepts matching bytes");
    check(accepted.size == bytes.size() && accepted.crc32 == identity.crc32,
          "identity reports size and CRC");

    const GameDataResult wrong_size = verify_game_data(iso, {identity.size + 1, identity.crc32});
    check(!wrong_size.ok && wrong_size.error == "identity_size_mismatch",
          "size mismatch is named");

    const GameDataResult wrong_crc = verify_game_data(iso, {identity.size, identity.crc32 ^ 1u});
    check(!wrong_crc.ok && wrong_crc.error == "identity_crc_mismatch",
          "CRC mismatch is named");

    std::vector<std::uint8_t> corrupted = bytes;
    corrupted[123] ^= 0xFFu;
    write_file(iso, corrupted);
    const GameDataResult corrupt = verify_game_data(iso, identity);
    check(!corrupt.ok && corrupt.error == "identity_crc_mismatch",
          "corrupted content is rejected");

    const GameDataResult missing = verify_game_data(directory / "missing.iso", identity);
    check(!missing.ok && missing.error == "game_data_open_failed",
          "missing file is named");

    std::error_code error;
    fs::remove_all(directory, error);
}

void discovery_and_selection() {
    const fs::path directory = make_temp_dir();
    const std::vector<std::uint8_t> bytes = pattern(4096, 3);
    const GameIdentity identity = identity_of(bytes);

    fs::create_directories(directory / "external" / "game");
    fs::create_directories(directory / "internal" / "game");
    write_file(directory / "external" / "game" / "b.ISO", bytes);
    write_file(directory / "external" / "game" / "a.iso", bytes);
    write_file(directory / "external" / "game" / "notes.txt", pattern(64, 1));
    write_file(directory / "external" / "top.iso", bytes);

    const std::vector<GameDataSource> found =
        find_game_data({directory / "internal", directory / "external"});
    check(found.size() == 2, "only game/*.iso files are discovered");
    check(found[0].path.filename() == "a.iso" && found[1].path.filename() == "b.ISO",
          "discovery is sorted and case-insensitive");

    const GameDataResult selected =
        select_game_data({directory / "internal", directory / "external"}, identity);
    check(selected.ok && selected.path.filename() == "a.iso",
          "selection uses the first valid candidate");
    check(selected.origin == (directory / "external").string(), "selection keeps the origin");

    write_file(directory / "external" / "game" / "a.iso", pattern(4096, 99));
    const GameDataResult fallback =
        select_game_data({directory / "internal", directory / "external"}, identity);
    check(fallback.ok && fallback.path.filename() == "b.ISO",
          "selection skips a corrupt candidate");

    fs::create_directories(directory / "empty");
    const GameDataResult none = select_game_data({directory / "empty"}, identity);
    check(!none.ok && none.error == "no_game_data", "empty directories are named");

    std::error_code error;
    fs::remove_all(directory, error);
}

void asset_stamp() {
    check(version_stamp_matches("1\n", "1"), "asset stamp ignores trailing whitespace");
    check(version_stamp_matches("  2  ", "2"), "asset stamp ignores surrounding whitespace");
    check(!version_stamp_matches("1", "2"), "asset stamp mismatch is detected");
    check(!version_stamp_matches("", "1"), "missing asset stamp is a mismatch");
}

// The identity gate runs on the SDL thread, whose stack is about one megabyte.
// A verifier that keeps its scratch space on the stack eats that whole budget
// and leaves no room for the calls it makes afterwards, so the gate has to fit
// well below it. The check runs in a child process on a narrowed thread: the
// overflow is a SIGSEGV (not an exception), and a child keeps it reportable.
constexpr std::size_t kIdentityStackBudget = 512u * 1024u;

struct VerificationRun {
    std::filesystem::path path;
    GameIdentity identity;
    bool ok = false;
};

void *run_verification(void *argument) {
    auto *run = static_cast<VerificationRun *>(argument);
    const GameDataResult result = verify_game_data(run->path, run->identity);
    run->ok = result.ok;
    return nullptr;
}

void identity_fits_a_small_stack() {
    const fs::path directory = make_temp_dir();
    const std::vector<std::uint8_t> bytes = pattern(64u * 1024u, 11);
    const fs::path iso = directory / "game.iso";
    write_file(iso, bytes);

    const pid_t child = fork();
    check(child >= 0, "the small-stack check can fork");
    if (child == 0) {
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        const int sized = pthread_attr_setstacksize(&attributes, kIdentityStackBudget);
        VerificationRun run{iso, identity_of(bytes)};
        pthread_t thread{};
        const int created =
            sized == 0 ? pthread_create(&thread, &attributes, run_verification, &run) : sized;
        if (created == 0) pthread_join(thread, nullptr);
        _exit(created == 0 && run.ok ? 0 : 1);
    }

    int status = 0;
    waitpid(child, &status, 0);
    std::error_code error;
    fs::remove_all(directory, error);
    if (WIFSIGNALED(status)) {
        throw std::runtime_error("identity gate overflows a " +
                                 std::to_string(kIdentityStackBudget / 1024u) +
                                 " KiB stack (signal " + std::to_string(WTERMSIG(status)) + ")");
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "identity gate completes within a 512 KiB stack");
}

} // namespace

int main() {
    try {
        identity_gate();
        discovery_and_selection();
        asset_stamp();
        identity_fits_a_small_stack();
        std::cout << "game_data_tests: OK\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}

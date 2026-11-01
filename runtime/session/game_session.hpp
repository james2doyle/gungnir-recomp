#pragma once

// Shared bounded game session used by the Linux boot runner and the Android
// runtime (T13a). One implementation, no platform fork.

#include "session/session_report.hpp"

#include "audio/audio_sink.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace psprecomp {
class Runtime;
}

namespace gungnir::session {

// True when the binary was built with a generated AOT corpus. A false value is
// a named startup failure, never an empty run.
[[nodiscard]] bool generated_corpus_present() noexcept;

struct Config {
    std::filesystem::path elf_path;
    std::filesystem::path nids_path;
    std::filesystem::path disc_path;
    std::filesystem::path memory_stick;
    std::string boot_path{"disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"};
    std::uint32_t load_base{0x08804000u};
    std::uint64_t max_dispatches{50'000'000u};
    double wall_seconds{0.0};
    // Report a guest thread that stops making progress (blocked host call).
    // Zero disables the watchdog.
    double stall_seconds{0.0};
    bool stop_at_flip{true};
    bool pacing{false};
};

class GameSession {
public:
    struct Impl;

    GameSession();
    ~GameSession();

    GameSession(const GameSession &) = delete;
    GameSession &operator=(const GameSession &) = delete;

    // Loads the ELF, registers the generated corpus and the HLE boot set, and
    // arms the bounds. Returns false with a named error; nothing runs.
    bool start(const Config &config, gungnir::audio::AudioSink *sink, std::string &error);

    // Blocking. Call from a worker thread on Android. A stop request is
    // observed by the runtime heartbeat on the guest thread.
    void run();

    void request_stop();
    void shutdown();

    [[nodiscard]] const Result &result() const;
    [[nodiscard]] bool started() const;

    // Receives at most one line when the stall watchdog fires. Must be set
    // before start(); the sink runs on the watchdog thread.
    void set_diagnostic_sink(std::function<void(std::string_view)> sink);
    [[nodiscard]] psprecomp::Runtime &runtime();

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::session

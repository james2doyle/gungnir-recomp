// T15c slice 3 — HLE state + derived rebuild (red first).
//
// Usage: snapshot_hle_tests
// Exit 0 on the asserted contract, 1 on a failed contract.

#include "session/snapshot.hpp"

#include "audio/audio_sink.hpp"
#include "audio/sas_core.hpp"
#include "ge/state_machine.hpp"
#include "hle/hle_atrac.hpp"
#include "hle/hle_audio.hpp"
#include "hle/hle_common.hpp"
#include "hle/hle_media.hpp"
#include "hle/hle_module.hpp"
#include "hle/hle_utility.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/common.hpp"
#include "psprecomp/runtime.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using gungnir::hle::AllegrexContext;
using gungnir::hle::HleRegistrar;
using gungnir::hle::Kernel;
using gungnir::hle::Runtime;

int failures = 0;

void check(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

class StubSink final : public gungnir::audio::AudioSink {
public:
    bool submit(const gungnir::audio::AudioBuffer &) override { return true; }
};

struct HleEnv {
    static constexpr std::uint32_t kText = 0x08810000u;
    static constexpr std::uint32_t kOutput = 0x08811000u;
    static constexpr std::uint32_t kReturnAddress = 0x08822000u;

    Runtime runtime{32u * 1024u * 1024u};
    Kernel &kernel = gungnir::hle::kernel();
    StubSink sink;
    std::unique_ptr<HleRegistrar> hle;
    std::map<std::pair<std::string, std::string>, std::uint32_t> nids_by_name;
    std::filesystem::path ms_dir =
        std::filesystem::temp_directory_path() / "gungnir_hle_ms";

    HleEnv() {
        kernel = Kernel{};
        kernel.install(runtime, 0x08801000u, 0x08820000u);
        kernel.set_pacing_enabled(false);
        gungnir::audio::set_audio_sink(&sink);
        std::filesystem::create_directories(ms_dir);
        runtime.nids().load_csv(GUNGNIR_SNAPSHOT_NIDS_CSV);
        hle = std::make_unique<HleRegistrar>(runtime);
        for (const auto &symbol : runtime.nids().all())
            nids_by_name[{symbol.library, symbol.name}] = symbol.nid;
        gungnir::hle::reset_audio_state();
        gungnir::hle::reset_atrac_state();
        gungnir::hle::reset_media_state();
        gungnir::hle::reset_modules();
        gungnir::hle::reset_utility_state();
        gungnir::audio::reset_sas_cores();
        gungnir::hle::register_sysmem(*hle);
        gungnir::hle::register_threadman(*hle);
        gungnir::hle::register_system(*hle);
        gungnir::hle::register_audio(*hle);
        gungnir::hle::register_io(*hle, {}, ms_dir);
        gungnir::hle::register_atrac(*hle);
        gungnir::hle::register_media(*hle);
        gungnir::hle::register_utility(*hle);
        AllegrexContext &ctx = runtime.cpu();
        kernel.start_loader_thread(ctx, 0x08820000u, 0u);
    }

    ~HleEnv() {
        gungnir::audio::set_audio_sink(nullptr);
        kernel = Kernel{};
    }

    std::uint32_t call(const std::string &library, const std::string &name,
        std::initializer_list<std::uint32_t> args = {}) {
        AllegrexContext &ctx = runtime.cpu();
        for (unsigned i = 0; i < 8u; ++i) ctx.set_gpr(i + 4u, 0u);
        unsigned index = 0;
        for (const std::uint32_t value : args) ctx.set_gpr(4u + index++, value);
        ctx.set_gpr(31, kReturnAddress);
        const auto found = nids_by_name.find({library, name});
        if (found == nids_by_name.end()) throw std::runtime_error("no NID for " + library + "::" + name);
        runtime.invoke_import(library, found->second, ctx);
        return ctx.gpr[2];
    }
};

std::array<std::uint8_t, 32> filled(std::uint8_t value) {
    std::array<std::uint8_t, 32> out{};
    out.fill(value);
    return out;
}

} // namespace

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "gungnir_snapshot_hle_tests";
    std::filesystem::create_directories(dir);
    const auto corpus = filled(0x11u);
    const auto config = filled(0x22u);

    // Parked blocking output: the frame thread parks in
    // sceAudioOutputPannedBlocking while the channel drains (pre-battle stall
    // shape). Save mid-schedule via the poll hook, then restore into a fresh
    // env and prove the waiter is resumed-or-reported, never dangling.
    std::filesystem::path parked_path;
    std::uint64_t parked_deadline = 0u;
    std::uint64_t parked_now = 0u;
    std::uint32_t parked_channel = 0xFFFFFFFFu;
    {
        HleEnv env;
        const std::uint32_t channel = env.call("sceAudio", "sceAudioChReserve", {0xFFFFFFFFu, 0x40u, 0u});
        check(channel < 8u, "channel reserved");
        const std::uint32_t first_nb =
            env.call("sceAudio", "sceAudioOutputPanned", {channel, 0x8000u, 0x8000u, HleEnv::kOutput});
        const std::uint32_t first =
            env.call("sceAudio", "sceAudioOutputPannedBlocking", {channel, 0x8000u, 0x8000u, HleEnv::kOutput});
        check(first == 0x40u, "first buffer handed over");
        const std::uint64_t now_before = env.kernel.now_us();
        bool saved = false;
        env.kernel.set_poll_hook([&] {
            if (saved) return;
            for (const auto &[uid, thread] : env.kernel.threads()) {
                if (thread->wait.type == gungnir::hle::WaitType::Audio && thread->wait.deadline_us) {
                    parked_deadline = *thread->wait.deadline_us;
                    parked_now = env.kernel.now_us();
                    parked_channel = static_cast<std::uint32_t>(thread->wait.object) - 1u;
                    gungnir::session::SnapshotError error{};
                    gungnir::session::ClockState clock{
                        env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
                    saved = gungnir::session::snapshot_save_full(parked_path = dir / "parked.gsnap", corpus,
                        config, env.runtime.memory(), env.runtime.cpu(), clock, env.kernel, error);
                    if (!saved) std::cerr << "FAIL: poll-hook save: " + error.message + "\n";
                    env.kernel.set_poll_hook({});
                    return;
                }
            }
        });
        const std::uint32_t second =
            env.call("sceAudio", "sceAudioOutputPannedBlocking", {channel, 0x8000u, 0x8000u, HleEnv::kOutput});
        env.kernel.set_poll_hook({});
        check(saved, "snapshot saved while thread parked");
        check(second == 0x40u, "parked output completes after drain");
        check(parked_deadline == now_before + 1452u, "park deadline recorded");
        check(parked_now == now_before, "park clock matches");
        check(parked_channel == channel, "park names the draining channel");
    }

    {
        HleEnv env;
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{};
        check(gungnir::session::snapshot_load_full(
                  parked_path, corpus, config, env.runtime.memory(), env.runtime.cpu(), clock, env.kernel, error),
            "parked load ok: " + error.message);
        const std::uint32_t rest = env.call("sceAudio", "sceAudioGetChannelRestLen", {parked_channel});
        check(rest == 0x80u, "restored channel draining with parked waiter (rest==128), got " + std::to_string(rest));
        bool found = false;
        for (const auto &[uid, thread] : env.kernel.threads()) {
            if (thread->wait.type == gungnir::hle::WaitType::Audio && thread->wait.deadline_us &&
                *thread->wait.deadline_us == parked_deadline) {
                found = true;
                check(thread->wait.object == static_cast<std::int32_t>(parked_channel + 1u),
                    "audio wait names the draining channel");
            }
        }
        check(found, "restored waiter present with absolute deadline (resumed-or-reported, never dangling)");
    }

    // Corrupt waiter marker: a channel parked on a thread that does not exist
    // must fail load with a named error, leaving the target untouched.
    {
        std::vector<std::uint8_t> bytes;
        std::string message;
        check(gungnir::hle::export_snapshot_audio(bytes, message), "audio export ok: " + message);
        check(!bytes.empty(), "audio section non-empty");
        // waiting-uid field of channel 0 is bytes [8..12) of its 52-byte record.
        constexpr std::size_t kRecord = 52u;
        constexpr std::size_t kWaitingOff = 8u;
        check(bytes.size() >= kRecord, "audio section holds a full record");
        if (bytes.size() >= kRecord) {
            bytes[kWaitingOff + 0u] = 0x78u;
            bytes[kWaitingOff + 1u] = 0x56u;
            bytes[kWaitingOff + 2u] = 0x34u;
            bytes[kWaitingOff + 3u] = 0x12u;
        }
        HleEnv env;
        const std::uint64_t flips_before = env.kernel.now_us();
        check(!gungnir::hle::import_snapshot_audio(bytes, env.kernel, message), "dangling waiter rejected");
        check(message.rfind("snapshot_dangling_wait:", 0) == 0, "dangling names snapshot_dangling_wait");
        check(env.kernel.now_us() == flips_before, "failed audio import leaves kernel untouched");
    }

    // Open descriptors: positions survive the round trip against the same
    // memory stick; a deleted backing file fails the load by name.
    std::filesystem::path fd_snapshot;
    std::uint32_t saved_fd = 0u;
    {
        HleEnv env;
        gungnir::hle::write_cstring(env.runtime.memory(), HleEnv::kText, "ms0:/snap_hle_fd.bin", 64u);
        for (std::uint32_t i = 0u; i < 64u; ++i)
            env.runtime.memory().store8(HleEnv::kOutput + i, static_cast<std::uint8_t>(i * 3u + 1u));
        const std::uint32_t fd =
            env.call("IoFileMgrForUser", "sceIoOpen", {HleEnv::kText, 0x200u | 0x2u});
        check(fd >= 3u, "fd opened for write");
        saved_fd = fd;
        const std::uint32_t written =
            env.call("IoFileMgrForUser", "sceIoWrite", {fd, HleEnv::kOutput, 64u});
        check(written == 64u, "payload written");
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(fd_snapshot = dir / "fds.gsnap", corpus, config,
                  env.runtime.memory(), env.runtime.cpu(), clock, env.kernel, error),
            "fd save ok: " + error.message);
    }
    {
        HleEnv env;
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{};
        check(gungnir::session::snapshot_load_full(
                  fd_snapshot, corpus, config, env.runtime.memory(), env.runtime.cpu(), clock, env.kernel, error),
            "fd load ok: " + error.message);
        const std::uint32_t eof =
            env.call("IoFileMgrForUser", "sceIoRead", {saved_fd, HleEnv::kOutput + 128u, 64u});
        check(eof == 0u, "restored position is at end of file");
        check(env.call("IoFileMgrForUser", "sceIoLseek", {saved_fd, 0u, 0u, 0u, 0u}) == 0u,
            "seek to start");
        const std::uint32_t got =
            env.call("IoFileMgrForUser", "sceIoRead", {saved_fd, HleEnv::kOutput + 128u, 64u});
        check(got == 64u, "reread after seek returns payload");
        bool match = true;
        for (std::uint32_t i = 0u; i < 64u; ++i) {
            if (env.runtime.memory().load8(HleEnv::kOutput + 128u + i) != static_cast<std::uint8_t>(i * 3u + 1u))
                match = false;
        }
        check(match, "restored fd content matches");
    }
    {
        const std::filesystem::path host_file =
            std::filesystem::temp_directory_path() / "gungnir_hle_ms" / "snap_hle_fd.bin";
        std::error_code ec;
        std::filesystem::remove(host_file, ec);
        HleEnv env;
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{};
        check(!gungnir::session::snapshot_load_full(
                  fd_snapshot, corpus, config, env.runtime.memory(), env.runtime.cpu(), clock, env.kernel, error),
            "deleted backing file rejects load");
        check(error.code == gungnir::session::SnapshotError::kOpen, "missing file names snapshot_open_failed");
    }

    // SAS cores: voice cursors and envelopes round trip byte-identically.
    {
        HleEnv env;
        auto &core = gungnir::audio::sas_core(0);
        core.init(256u, 32u, 0u);
        core.set_voice(0u, HleEnv::kOutput, 1024u, false);
        core.set_pitch(0u, 0x2000u);
        core.set_volume(0u, 0x1000, 0x0800);
        core.set_noise(4u, 64);
        core.key_on(0u);
        std::vector<std::uint8_t> before;
        std::string message;
        check(gungnir::audio::export_snapshot_sas(before, message), "sas export ok: " + message);
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(dir / "sas.gsnap", corpus, config, env.runtime.memory(),
                  env.runtime.cpu(), clock, env.kernel, error),
            "sas save ok: " + error.message);
        HleEnv restored;
        gungnir::session::ClockState loaded{};
        check(gungnir::session::snapshot_load_full(dir / "sas.gsnap", corpus, config, restored.runtime.memory(),
                  restored.runtime.cpu(), loaded, restored.kernel, error),
            "sas load ok: " + error.message);
        std::vector<std::uint8_t> after;
        check(gungnir::audio::export_snapshot_sas(after, message), "sas re-export ok: " + message);
        check(before == after, "sas state byte-identical across round trip");
        check(gungnir::audio::sas_core(0).voice_playing(0u), "sas voice still keyed on");
    }

    // ATRAC IDs: track bytes + positions survive; corrupt bytes fail by name.
    {
        HleEnv env;
        std::vector<std::uint8_t> riff;
        const auto push_str = [&](const char *text) {
            riff.insert(riff.end(), text, text + 4u);
        };
        const auto push32 = [&](std::uint32_t value) {
            riff.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            riff.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
            riff.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
            riff.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
        };
        const auto push16 = [&](std::uint16_t value) {
            riff.push_back(static_cast<std::uint8_t>(value & 0xFFu));
            riff.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
        };
        constexpr std::uint8_t kGuid[16] = {0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44,
            0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
        push_str("RIFF");
        push32(192u + 76u);
        push_str("WAVE");
        push_str("fmt ");
        push32(52u);
        push16(0xFFFEu);
        push16(1u);
        push32(44100u);
        push32(0u);
        push16(192u);
        push16(0u);
        for (int i = 0; i < 4; ++i) push16(0u);
        riff.insert(riff.end(), std::begin(kGuid), std::end(kGuid));
        push16(0u);
        push16(2u);
        for (int i = 0; i < 4; ++i) push16(0u);
        push_str("fact");
        push32(8u);
        push32(2048u);
        push32(0u);
        push_str("data");
        push32(192u);
        riff.insert(riff.end(), 192u, 0u);
        for (std::size_t i = 0u; i < riff.size(); ++i)
            env.runtime.memory().store8(HleEnv::kOutput + static_cast<std::uint32_t>(i), riff[i]);
        const std::uint32_t id = env.call(
            "sceAtrac3plus", "sceAtracSetDataAndGetID", {HleEnv::kOutput, static_cast<std::uint32_t>(riff.size())});
        check(id >= 1u && id <= 6u, "atrac id allocated");
        const std::uint32_t bare_id =
            env.call("sceAtrac3plus", "sceAtracGetAtracID", {0x00001000u});
        check(bare_id >= 1u && bare_id <= 6u && bare_id != id, "dataless atrac id allocated");
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(dir / "atrac.gsnap", corpus, config, env.runtime.memory(),
                  env.runtime.cpu(), clock, env.kernel, error),
            "atrac save ok: " + error.message);
        HleEnv restored;
        gungnir::session::ClockState loaded{};
        check(gungnir::session::snapshot_load_full(dir / "atrac.gsnap", corpus, config,
                  restored.runtime.memory(), restored.runtime.cpu(), loaded, restored.kernel, error),
            "atrac load ok: " + error.message);
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
        std::string message;
        check(gungnir::hle::export_snapshot_atrac(before, message), "atrac export ok");
        check(gungnir::hle::export_snapshot_atrac(after, message), "atrac re-export ok");
        check(before == after && !before.empty(), "atrac metadata byte-identical");
        std::vector<std::uint8_t> corrupt = before;
        if (corrupt.size() > 64u) {
            std::fill(corrupt.begin() + 32u, corrupt.begin() + 64u, 0u);
            check(!gungnir::hle::import_snapshot_atrac(corrupt, message), "corrupt atrac rejected");
            check(message.rfind("snapshot_open_failed:", 0) == 0, "corrupt atrac names snapshot_open_failed");
        } else {
            check(false, "atrac section big enough to corrupt");
        }
    }


    // GE lists + display: a stalled list resumes after restore; a completed
    // list stays done; the flip count round trips.
    {
        HleEnv env;
        env.runtime.memory().store32(HleEnv::kOutput, 0x0C000000u);
        env.runtime.memory().store32(HleEnv::kOutput + 64u, 0x0C000000u);
        const std::uint32_t done_id =
            env.call("sceGe_user", "sceGeListEnQueue", {HleEnv::kOutput, 0u, 0xFFFFFFFFu});
        check(env.call("sceGe_user", "sceGeListSync", {done_id}) == 0u, "completed list syncs done");
        const std::uint32_t stalled_id = env.call(
            "sceGe_user", "sceGeListEnQueue", {HleEnv::kOutput + 64u, HleEnv::kOutput + 64u, 0xFFFFFFFFu});
        check(env.call("sceGe_user", "sceGeListSync", {stalled_id}) == 2u, "stalled list syncs drawing");
        // In-flight draws are derived: saving mid-frame succeeds and drops them.
        constexpr std::uint32_t kVerts = HleEnv::kOutput + 1024u;
        for (std::uint32_t v = 0u; v < 3u; ++v) {
            const float coords[3] = {10.0f + static_cast<float>(v) * 20.0f, 20.0f, 0.0f};
            for (int c = 0; c < 3; ++c) {
                std::uint32_t bits = 0u;
                static_assert(sizeof(float) == 4u);
                __builtin_memcpy(&bits, &coords[c], 4u);
                env.runtime.memory().store32(
                    kVerts + v * 12u + static_cast<std::uint32_t>(c) * 4u, bits);
            }
        }
        constexpr std::uint32_t kPrimList = HleEnv::kOutput + 256u;
        env.runtime.memory().store32(kPrimList + 0u, 0x10000000u | (0x08u << 16u));
        env.runtime.memory().store32(kPrimList + 4u, 0x01000000u | (kVerts & 0x00FFFFFFu));
        env.runtime.memory().store32(kPrimList + 8u, 0x12000000u | 0x800180u);
        env.runtime.memory().store32(kPrimList + 12u, 0x04000000u | 0x30003u);
        env.runtime.memory().store32(kPrimList + 16u, 0x0C000000u);
        const std::uint32_t prim_id =
            env.call("sceGe_user", "sceGeListEnQueue", {kPrimList, 0u, 0xFFFFFFFFu});
        check(env.call("sceGe_user", "sceGeListSync", {prim_id}) == 0u, "prim list completes");
        check(gungnir::hle::ge_pending_draw_count() == 1u, "one draw pending presentation");
        check(env.call("sceDisplay", "sceDisplaySetFrameBuf",
                      {0x04000000u, 512u, 3u, 1u}) == 0u,
            "flip accepted");
        check(gungnir::hle::display_state().flips == 1u, "flip counted");
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(dir / "ge.gsnap", corpus, config, env.runtime.memory(),
                  env.runtime.cpu(), clock, env.kernel, error),
            "ge save ok: " + error.message);
        HleEnv restored;
        gungnir::session::ClockState loaded{};
        check(gungnir::session::snapshot_load_full(dir / "ge.gsnap", corpus, config,
                  restored.runtime.memory(), restored.runtime.cpu(), loaded, restored.kernel, error),
            "ge load ok: " + error.message);
        check(gungnir::hle::display_state().flips == 1u, "display flips restored");
        check(gungnir::hle::display_state().framebuffer == 0x04000000u, "framebuffer restored");
        check(restored.call("sceGe_user", "sceGeListSync", {done_id}) == 0u, "done list stays done");
        check(restored.call("sceGe_user", "sceGeListSync", {stalled_id}) == 2u, "stalled list still drawing");
        check(restored.call("sceGe_user", "sceGeListUpdateStallAddr", {stalled_id, 0u}) == 0u,
            "stall released");
        check(restored.call("sceGe_user", "sceGeListSync", {stalled_id}) == 0u, "resumed list completes");
        check(gungnir::hle::ge_pending_draw_count() == 0u, "in-flight draws dropped on load");
    }
    // StateMachine registers round trip through the public codec.
    {
        gungnir::ge::StateMachine before;
        const gungnir::ge::Command ztest =
            gungnir::ge::decode_command(0u, (0x23u << 24u) | 1u);
        before.command(ztest, [](std::uint32_t, std::uint8_t *, std::size_t) { return false; });
        check(before.registers()[0x23] == 1u, "statemachine records state");
        std::vector<std::uint8_t> bytes;
        before.export_snapshot(bytes);
        gungnir::ge::StateMachine after;
        std::string error;
        check(after.import_snapshot(bytes, error), "statemachine import ok: " + error);
        check(after.registers() == before.registers(), "statemachine registers identical");
        check(after.draw_count() == before.draw_count(), "statemachine counters identical");
    }

    // Module registry: records (minus corpus installers) round trip.
    {
        HleEnv env;
        gungnir::hle::register_main_module("gungnir_main", 0x08804000u, 0x100000u, 0x08900000u);
        const gungnir::hle::SceUID overlay =
            gungnir::hle::add_module(env.runtime, "overlay", 0x08910000u, 0x1000u, 0x08900000u, {});
        check(overlay > 0, "overlay module added");
        check(gungnir::hle::module_count() == 2u, "two modules registered");
        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(dir / "modules.gsnap", corpus, config, env.runtime.memory(),
                  env.runtime.cpu(), clock, env.kernel, error),
            "module save ok: " + error.message);
        HleEnv restored;
        gungnir::session::ClockState loaded{};
        check(gungnir::session::snapshot_load_full(dir / "modules.gsnap", corpus, config,
                  restored.runtime.memory(), restored.runtime.cpu(), loaded, restored.kernel, error),
            "module load ok: " + error.message);
        check(gungnir::hle::module_count() == 2u, "module count restored");
        const gungnir::hle::Module *main = gungnir::hle::find_module(0x01000001);
        check(main != nullptr && main->base == 0x08804000u, "main module restored");
        const gungnir::hle::Module *found = gungnir::hle::find_module(overlay);
        check(found != nullptr && found->name == "overlay", "overlay module restored");
    }

    // Utility status machines: an in-flight savedata op keeps its status.
    {
        HleEnv env;
        constexpr std::uint32_t kParam = 0x08813000u;
        env.runtime.memory().store32(kParam + 0x00u, 0x600u);
        env.runtime.memory().store32(kParam + 0x30u, 1u);
        gungnir::hle::write_cstring(env.runtime.memory(), kParam + 0x3Cu, "ULUS10592", 13u);
        gungnir::hle::write_cstring(env.runtime.memory(), kParam + 0x4Cu, "SAVE00001", 20u);
        gungnir::hle::write_cstring(env.runtime.memory(), kParam + 0x64u, "DATA.BIN", 13u);
        env.runtime.memory().store32(kParam + 0x60u, kParam + 0x400u);
        for (std::uint32_t i = 0u; i < 32u; ++i) env.runtime.memory().store8(kParam + 0x400u + i, 0u);
        check(env.call("sceUtility", "sceUtilitySavedataInitStart", {kParam}) == 0u, "savedata init accepted");

        gungnir::session::SnapshotError error{};
        gungnir::session::ClockState clock{
            env.kernel.now_us(), env.kernel.next_vblank_us(), env.kernel.vblank_count()};
        check(gungnir::session::snapshot_save_full(dir / "utility.gsnap", corpus, config, env.runtime.memory(),
                  env.runtime.cpu(), clock, env.kernel, error),
            "utility save ok: " + error.message);
        HleEnv restored;
        gungnir::session::ClockState loaded{};
        check(gungnir::session::snapshot_load_full(dir / "utility.gsnap", corpus, config,
                  restored.runtime.memory(), restored.runtime.cpu(), loaded, restored.kernel, error),
            "utility load ok: " + error.message);
        check(restored.call("sceUtility", "sceUtilitySavedataGetStatus", {}) == 2u,
            "utility machine resumes after restore");
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
        std::string message;
        check(gungnir::hle::export_snapshot_utility(before, message), "utility export ok");
        check(gungnir::hle::export_snapshot_utility(after, message), "utility re-export ok");
        check(before == after, "utility machines byte-identical");
    }

    if (failures != 0) {
        std::cerr << "snapshot_hle_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "snapshot_hle_tests: OK\n";
    return 0;
}

#pragma once

// Adapted from TeamGDB/Yakumo (MIT), profiles/mhp3rd/host/audio/sas_core.*
// at the pinned commit. Behaviour is unchanged; namespace, environment hooks
// and per-title tracing were removed for this runtime.
#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gungnir::audio {

inline constexpr std::uint32_t kSasMaxVoices = 32u;
// SAS envelope heights are 0..0x40000000 on hardware; the rate encoding it
// inherits from the SPU works in 15-bit steps, so the envelope is kept there
// and scaled up only when the guest asks to read it.
inline constexpr std::int32_t kEnvelopeMax = 0x7FFF;

enum class EnvelopeStage { Off, Attack, Decay, Sustain, Release };

struct SasVoice {
    // Source
    std::uint32_t address{};
    std::uint32_t size{};
    bool looping{};
    bool pcm{}; // raw 16-bit PCM instead of VAG
    bool noise{};
    std::uint32_t pitch{0x1000u};
    std::int32_t left{0x1000};
    std::int32_t right{0x1000};

    bool playing{};
    bool paused{};

    // VAG decoder position: `block` is a byte offset from `address`, `index`
    // the sample inside the 28-sample block it decoded, `phase` the 12-bit
    // fraction of a source sample the resampler has advanced past `index`.
    std::uint32_t block{};
    std::uint32_t loop_block{};
    std::int32_t index{28};
    std::uint32_t phase{};
    std::int32_t history1{};
    std::int32_t history2{};
    std::array<std::int16_t, 28> decoded{};
    std::int16_t previous{};
    std::int16_t current{};
    bool source_ended{};
    bool primed{};

    // Envelope
    bool have_adsr{};
    std::uint32_t adsr1{};
    std::uint32_t adsr2{};
    EnvelopeStage stage{EnvelopeStage::Off};
    std::int32_t envelope{};
    std::int32_t envelope_counter{};
};

// Software SAS: up to 32 VAG voices mixed at 44100 Hz. Reverb is not modelled,
// so the effect sends are accepted and ignored.
class SasCore {
public:
    void init(std::uint32_t grain, std::uint32_t max_voices, std::uint32_t output_mode);
    [[nodiscard]] std::uint32_t grain() const noexcept { return grain_; }
    [[nodiscard]] std::uint32_t output_mode() const noexcept { return output_mode_; }
    void set_grain(std::uint32_t grain) noexcept { grain_ = grain; }
    void set_output_mode(std::uint32_t mode) noexcept { output_mode_ = mode; }
    [[nodiscard]] bool voice_playing(std::uint32_t voice) const noexcept {
        return voice < kSasMaxVoices && voices_[voice].playing;
    }
    [[nodiscard]] bool voice_paused(std::uint32_t voice) const noexcept {
        return voice < kSasMaxVoices && voices_[voice].paused;
    }
    [[nodiscard]] std::uint32_t pause_flag() const noexcept {
        std::uint32_t flags = 0u;
        for (std::uint32_t voice = 0; voice < kSasMaxVoices; ++voice)
            if (voices_[voice].paused) flags |= 1u << voice;
        return flags;
    }

    void set_voice(std::uint32_t voice, std::uint32_t address, std::uint32_t size, bool looping);
    void set_voice_pcm(std::uint32_t voice, std::uint32_t address, std::uint32_t size, std::int32_t loop);
    void set_pitch(std::uint32_t voice, std::uint32_t pitch);
    void set_volume(std::uint32_t voice, std::int32_t left, std::int32_t right);
    void set_simple_adsr(std::uint32_t voice, std::uint32_t adsr1, std::uint32_t adsr2);
    void set_adsr(std::uint32_t voice, std::uint32_t flag, std::int32_t attack, std::int32_t decay,
        std::int32_t sustain, std::int32_t release);
    void set_adsr_mode(std::uint32_t voice, std::uint32_t flag, std::int32_t attack, std::int32_t decay,
        std::int32_t sustain, std::int32_t release);
    void set_sustain_level(std::uint32_t voice, std::int32_t level);
    void set_noise(std::uint32_t voice, std::int32_t frequency);
    void set_reverb_type(std::int32_t type) noexcept { reverb_.type = type; }
    void set_reverb_params(std::int32_t delay, std::int32_t feedback) noexcept {
        reverb_.delay = delay;
        reverb_.feedback = feedback;
    }
    void set_reverb_volume(std::int32_t left, std::int32_t right) noexcept {
        reverb_.left = left;
        reverb_.right = right;
    }
    void set_reverb_send(bool dry, bool wet) noexcept {
        reverb_.dry = dry;
        reverb_.wet = wet;
    }
    void set_pause(std::uint32_t mask, bool paused);
    void key_on(std::uint32_t voice);
    void key_off(std::uint32_t voice);
    // One bit per voice, set while the voice is *not* playing.
    [[nodiscard]] std::uint32_t end_flag() const noexcept;
    [[nodiscard]] std::int32_t envelope_height(std::uint32_t voice) const noexcept;
    // Renders `frames` stereo frames into `output`, overwriting it.
    void render(const psprecomp::GuestMemory &memory, std::int16_t *output, std::size_t frames);

    // Snapshot codec (T15c): deterministic fixed-size record (voices carry
    // VAG decoder cursors, so a restored core resumes mid-stream).
    void export_snapshot(std::vector<std::uint8_t> &out) const;
    bool import_snapshot(std::span<const std::uint8_t> bytes, std::string &error);

private:
    void advance_source(const psprecomp::GuestMemory &memory, SasVoice &voice);
    void decode_block(const psprecomp::GuestMemory &memory, SasVoice &voice);
    void step_envelope(SasVoice &voice);

    // Reverb is not modelled; the settings are kept so guest state round-trips.
    struct ReverbState {
        std::int32_t type{-1};
        std::int32_t delay{};
        std::int32_t feedback{};
        std::int32_t left{0x1000};
        std::int32_t right{0x1000};
        bool dry{true};
        bool wet{};
    };

    std::uint32_t grain_{256u};
    std::uint32_t max_voices_{kSasMaxVoices};
    std::uint32_t output_mode_{};
    ReverbState reverb_{};
    std::array<SasVoice, kSasMaxVoices> voices_{};
};

// One core per guest SAS handle. The handle is an argument to every call, so
// it is keyed rather than assumed.
[[nodiscard]] SasCore &sas_core(std::uint32_t handle);
void reset_sas_cores();

// Snapshot export/import over all cores (T15c). Import is fail-closed.
[[nodiscard]] bool export_snapshot_sas(std::vector<std::uint8_t> &bytes_out, std::string &error);
bool import_snapshot_sas(std::span<const std::uint8_t> bytes, std::string &error);

} // namespace gungnir::audio

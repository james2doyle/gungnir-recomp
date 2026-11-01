#include "audio/sas_core.hpp"

#include <algorithm>
#include <bit>
#include <map>

namespace gungnir::audio {
namespace {

// The five SPU ADPCM predictor coefficient pairs, in 1/64ths.
constexpr std::int32_t kPredictorA[5] = {0, 60, 115, 98, 122};
constexpr std::int32_t kPredictorB[5] = {0, 0, -52, -55, -60};

constexpr std::uint32_t kPitchUnity = 0x1000u;
constexpr std::uint32_t kPitchMax = 0x4000u;
constexpr std::int32_t kVolumeUnity = 0x1000;

std::map<std::uint32_t, SasCore> g_cores;

[[nodiscard]] std::int32_t clamp16(std::int32_t value) noexcept {
    return std::clamp(value, -32768, 32767);
}

struct EnvelopeRate {
    std::int32_t step{};
    std::int32_t period{1};
};

// The SPU rate encoding the SAS inherits: the low two bits pick the step and
// the rest a shift. Below a shift of 11 the envelope moves every sample by a
// larger step; above it, by the smallest step but only every 1<<(shift-11)
// samples. Exponential attack slows fourfold past three quarters of full
// scale; exponential decrease scales the step by the current level.
[[nodiscard]] EnvelopeRate decode_rate(
    std::uint32_t rate, bool increase, bool exponential, std::int32_t level) noexcept {
    const std::int32_t shift = static_cast<std::int32_t>((rate >> 2) & 0x1Fu);
    const auto step_index = static_cast<std::int32_t>(rate & 3u);
    EnvelopeRate result{};
    result.step = increase ? (7 - step_index) : (-8 + step_index);
    if (shift > 11)
        result.period = 1 << (shift - 11);
    else
        result.step <<= (11 - shift);
    if (exponential) {
        if (increase && level > 0x6000)
            result.period *= 4;
        else if (!increase)
            result.step = static_cast<std::int32_t>(static_cast<std::int64_t>(result.step) * level / 0x8000);
    }
    if (result.step == 0) result.step = increase ? 1 : -1;
    return result;
}

} // namespace

void SasCore::init(std::uint32_t grain, std::uint32_t max_voices, std::uint32_t output_mode) {
    grain_ = grain != 0u && grain <= 2048u ? grain : 256u;
    max_voices_ = std::clamp(max_voices, 1u, kSasMaxVoices);
    output_mode_ = output_mode;
    voices_ = {};
}

void SasCore::set_voice(std::uint32_t voice, std::uint32_t address, std::uint32_t size, bool looping) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    v.address = address;
    v.size = size & ~0xFu;
    v.looping = looping;
    v.pcm = false;
    v.noise = false;
    v.loop_block = 0u;
    v.source_ended = address == 0u || v.size < 16u;
}

void SasCore::set_voice_pcm(std::uint32_t voice, std::uint32_t address, std::uint32_t size, std::int32_t loop) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    v.address = address;
    v.size = size;
    v.looping = loop >= 0;
    v.pcm = true;
    v.noise = false;
    v.loop_block = 0u;
    v.source_ended = address == 0u || size < 2u;
}

void SasCore::set_pitch(std::uint32_t voice, std::uint32_t pitch) {
    if (voice >= kSasMaxVoices) return;
    voices_[voice].pitch = std::min(pitch, kPitchMax);
}

void SasCore::set_volume(std::uint32_t voice, std::int32_t left, std::int32_t right) {
    if (voice >= kSasMaxVoices) return;
    voices_[voice].left = std::clamp(left, -kVolumeUnity, kVolumeUnity);
    voices_[voice].right = std::clamp(right, -kVolumeUnity, kVolumeUnity);
}

void SasCore::set_simple_adsr(std::uint32_t voice, std::uint32_t adsr1, std::uint32_t adsr2) {
    if (voice >= kSasMaxVoices) return;
    voices_[voice].adsr1 = adsr1 & 0xFFFFu;
    voices_[voice].adsr2 = adsr2 & 0xFFFFu;
    voices_[voice].have_adsr = true;
}

void SasCore::set_adsr(std::uint32_t voice, std::uint32_t flag, std::int32_t attack, std::int32_t decay,
    std::int32_t sustain, std::int32_t release) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    if ((flag & 0x1u) != 0u) v.adsr1 = (v.adsr1 & ~0x7F00u) | ((static_cast<std::uint32_t>(attack) & 0x7Fu) << 8u);
    if ((flag & 0x2u) != 0u) v.adsr1 = (v.adsr1 & ~0x00F0u) | ((static_cast<std::uint32_t>(decay) & 0xFu) << 4u);
    if ((flag & 0x4u) != 0u) v.adsr2 = (v.adsr2 & ~0x1FC0u) | ((static_cast<std::uint32_t>(sustain) & 0x7Fu) << 6u);
    if ((flag & 0x8u) != 0u) v.adsr2 = (v.adsr2 & ~0x001Fu) | (static_cast<std::uint32_t>(release) & 0x1Fu);
    v.have_adsr = true;
}

void SasCore::set_adsr_mode(std::uint32_t voice, std::uint32_t flag, std::int32_t attack, std::int32_t decay,
    std::int32_t sustain, std::int32_t release) {
    if (voice >= kSasMaxVoices) return;
    (void)decay;
    SasVoice &v = voices_[voice];
    if ((flag & 0x1u) != 0u) {
        if (attack == 2 || attack == 4)
            v.adsr1 |= 0x8000u;
        else
            v.adsr1 &= ~0x8000u;
    }
    if ((flag & 0x4u) != 0u) {
        v.adsr2 &= ~0xC000u;
        switch (sustain) {
        case 1:
        case 5: v.adsr2 |= 0x4000u; break;
        case 2:
        case 4: v.adsr2 |= 0x8000u; break;
        case 3: v.adsr2 |= 0xC000u; break;
        default: break;
        }
    }
    if ((flag & 0x8u) != 0u) {
        if (release == 3)
            v.adsr2 |= 0x20u;
        else
            v.adsr2 &= ~0x20u;
    }
    v.have_adsr = true;
}

void SasCore::set_sustain_level(std::uint32_t voice, std::int32_t level) {
    if (voice >= kSasMaxVoices) return;
    voices_[voice].adsr1 = (voices_[voice].adsr1 & ~0xFu) | (static_cast<std::uint32_t>(level) & 0xFu);
}

void SasCore::set_noise(std::uint32_t voice, std::int32_t frequency) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    v.noise = true;
    v.source_ended = false;
    (void)frequency;
}

void SasCore::set_pause(std::uint32_t mask, bool paused) {
    for (std::uint32_t voice = 0; voice < kSasMaxVoices; ++voice)
        if ((mask & (1u << voice)) != 0u) voices_[voice].paused = paused;
}

void SasCore::key_on(std::uint32_t voice) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    v.block = 0u;
    v.loop_block = 0u;
    v.index = 28;
    v.phase = 0u;
    v.history1 = 0;
    v.history2 = 0;
    v.previous = 0;
    v.current = 0;
    v.source_ended = !v.noise && (v.address == 0u || v.size < (v.pcm ? 2u : 16u));
    v.primed = false;
    v.paused = false;
    v.playing = !v.source_ended;
    v.envelope_counter = 0;
    if (v.have_adsr) {
        v.stage = EnvelopeStage::Attack;
        v.envelope = 0;
    } else {
        v.stage = EnvelopeStage::Sustain;
        v.envelope = kEnvelopeMax;
    }
}

void SasCore::key_off(std::uint32_t voice) {
    if (voice >= kSasMaxVoices) return;
    SasVoice &v = voices_[voice];
    if (!v.playing) return;
    if (v.have_adsr) {
        v.stage = EnvelopeStage::Release;
        v.envelope_counter = 0;
    } else {
        v.playing = false;
        v.stage = EnvelopeStage::Off;
        v.envelope = 0;
    }
}

std::uint32_t SasCore::end_flag() const noexcept {
    std::uint32_t flags = 0u;
    for (std::uint32_t voice = 0; voice < kSasMaxVoices; ++voice)
        if (!voices_[voice].playing) flags |= 1u << voice;
    return flags;
}

std::int32_t SasCore::envelope_height(std::uint32_t voice) const noexcept {
    if (voice >= kSasMaxVoices) return 0;
    return voices_[voice].envelope << 15;
}

void SasCore::decode_block(const psprecomp::GuestMemory &memory, SasVoice &voice) {
    if (voice.block + 16u > voice.size) {
        voice.source_ended = true;
        return;
    }
    const std::uint8_t *block = memory.raw_pointer(voice.address + voice.block, 16u);
    if (block == nullptr) {
        voice.source_ended = true;
        return;
    }
    const std::uint32_t flags = block[1];
    // A terminator block carries no samples; everything else is decoded and the
    // flags only decide where playback goes next.
    if (flags == 7u) {
        voice.source_ended = true;
        return;
    }
    if ((flags & 4u) != 0u) voice.loop_block = voice.block;

    std::int32_t shift = block[0] & 0x0F;
    std::int32_t predictor = block[0] >> 4;
    if (predictor > 4) predictor = 0;
    if (shift > 12) shift = 9;
    for (std::int32_t sample = 0; sample < 28; ++sample) {
        const std::uint8_t byte = block[2 + sample / 2];
        const std::int32_t nibble = (sample & 1) != 0 ? (byte >> 4) : (byte & 0x0F);
        std::int32_t value = static_cast<std::int16_t>(nibble << 12) >> shift;
        value += (voice.history1 * kPredictorA[predictor] + voice.history2 * kPredictorB[predictor]) >> 6;
        value = clamp16(value);
        voice.decoded[static_cast<std::size_t>(sample)] = static_cast<std::int16_t>(value);
        voice.history2 = voice.history1;
        voice.history1 = value;
    }
    voice.index = 0;

    if ((flags & 1u) != 0u) {
        // Past this block playback either repeats or stops; a block offset at
        // the end of the sample makes the next decode report the end.
        voice.block = (flags & 2u) != 0u ? voice.loop_block : voice.size;
    } else {
        voice.block += 16u;
        if (voice.block + 16u > voice.size && voice.looping) voice.block = voice.loop_block;
    }
}

void SasCore::advance_source(const psprecomp::GuestMemory &memory, SasVoice &voice) {
    voice.previous = voice.current;
    if (voice.noise) {
        voice.current = 0;
        return;
    }
    if (voice.pcm) {
        const std::uint32_t offset = voice.block;
        if (offset + 2u > voice.size) {
            if (voice.looping) {
                voice.block = 0u;
            } else {
                voice.source_ended = true;
                voice.current = 0;
                return;
            }
        }
        const std::uint8_t *sample = memory.raw_pointer(voice.address + voice.block, 2u);
        voice.current =
            sample != nullptr ? static_cast<std::int16_t>(sample[0] | (sample[1] << 8)) : static_cast<std::int16_t>(0);
        voice.block += 2u;
        return;
    }
    if (voice.index >= 28) {
        decode_block(memory, voice);
        if (voice.source_ended) {
            voice.current = 0;
            return;
        }
    }
    voice.current = voice.decoded[static_cast<std::size_t>(voice.index++)];
}

void SasCore::step_envelope(SasVoice &voice) {
    if (voice.stage == EnvelopeStage::Off) return;
    // Without an ADSR the guest drives the level with SetVolume alone, so the
    // envelope stays wide open.
    if (!voice.have_adsr) return;
    if (--voice.envelope_counter > 0) return;

    const std::uint32_t adsr1 = voice.adsr1;
    const std::uint32_t adsr2 = voice.adsr2;
    EnvelopeRate rate{};
    switch (voice.stage) {
    case EnvelopeStage::Attack:
        rate = decode_rate((adsr1 >> 8) & 0x7Fu, true, (adsr1 & 0x8000u) != 0u, voice.envelope);
        break;
    case EnvelopeStage::Decay:
        // Decay has no step field: the shift alone, always an exponential fall.
        rate = decode_rate(((adsr1 >> 4) & 0x0Fu) << 2u, false, true, voice.envelope);
        break;
    case EnvelopeStage::Sustain:
        rate = decode_rate((adsr2 >> 6) & 0x7Fu, (adsr2 & 0x4000u) == 0u, (adsr2 & 0x8000u) != 0u, voice.envelope);
        break;
    case EnvelopeStage::Release:
        rate = decode_rate((adsr2 & 0x1Fu) << 2u, false, (adsr2 & 0x20u) != 0u, voice.envelope);
        break;
    case EnvelopeStage::Off:
        return;
    }
    voice.envelope_counter = rate.period;
    voice.envelope = std::clamp(voice.envelope + rate.step, 0, kEnvelopeMax);

    switch (voice.stage) {
    case EnvelopeStage::Attack:
        if (voice.envelope >= kEnvelopeMax) {
            voice.envelope = kEnvelopeMax;
            voice.stage = EnvelopeStage::Decay;
            voice.envelope_counter = 0;
        }
        break;
    case EnvelopeStage::Decay: {
        const std::int32_t sustain =
            std::min<std::int32_t>((static_cast<std::int32_t>(adsr1 & 0x0Fu) + 1) * 0x800, kEnvelopeMax);
        if (voice.envelope <= sustain) {
            voice.envelope = sustain;
            voice.stage = EnvelopeStage::Sustain;
            voice.envelope_counter = 0;
        }
        break;
    }
    case EnvelopeStage::Release:
        if (voice.envelope <= 0) {
            voice.envelope = 0;
            voice.stage = EnvelopeStage::Off;
            voice.playing = false;
        }
        break;
    default:
        break;
    }
}

void SasCore::render(const psprecomp::GuestMemory &memory, std::int16_t *output, std::size_t frames) {
    std::fill_n(output, frames * 2u, static_cast<std::int16_t>(0));
    for (std::uint32_t index = 0; index < max_voices_; ++index) {
        SasVoice &voice = voices_[index];
        if (!voice.playing || voice.paused) continue;

        // Keying on leaves the decoder empty, so the first two source samples
        // that the interpolator needs are fetched here.
        if (!voice.primed) {
            voice.primed = true;
            advance_source(memory, voice);
            advance_source(memory, voice);
        }

        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::int32_t span = voice.current - voice.previous;
            const std::int32_t sample = voice.previous + ((span * static_cast<std::int32_t>(voice.phase)) >> 12);
            step_envelope(voice);
            const std::int32_t scaled = (sample * voice.envelope) >> 15;
            const std::size_t slot = frame * 2u;
            output[slot] = static_cast<std::int16_t>(clamp16(output[slot] + ((scaled * voice.left) >> 12)));
            output[slot + 1u] = static_cast<std::int16_t>(clamp16(output[slot + 1u] + ((scaled * voice.right) >> 12)));

            // The previous sample is still to be played when a non-looping
            // source ends: stop only after mixing it. (Upstream Yakumo dropped
            // that final sample; the contract pins a full 28-sample block.)
            if (voice.source_ended) {
                voice.playing = false;
                voice.stage = EnvelopeStage::Off;
                voice.envelope = 0;
                break;
            }
            voice.phase += voice.pitch;
            while (voice.phase >= kPitchUnity) {
                voice.phase -= kPitchUnity;
                advance_source(memory, voice);
                if (voice.source_ended) break;
            }
            if (!voice.playing) break;
        }
    }
}

SasCore &sas_core(std::uint32_t handle) {
    return g_cores[handle];
}

void reset_sas_cores() {
    g_cores.clear();
}

namespace {

void sput_u32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((value >> 24u) & 0xFFu));
}

void sput_i16(std::vector<std::uint8_t> &out, std::int16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

struct SReader {
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

    [[nodiscard]] std::int16_t i16() {
        if (left < 2u) {
            ok = false;
            return 0;
        }
        const std::int16_t v =
            static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8u));
        p += 2;
        left -= 2;
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

void sput_voice(std::vector<std::uint8_t> &out, const SasVoice &voice) {
    sput_u32(out, voice.address);
    sput_u32(out, voice.size);
    out.push_back(voice.looping ? 1u : 0u);
    out.push_back(voice.pcm ? 1u : 0u);
    out.push_back(voice.noise ? 1u : 0u);
    sput_u32(out, voice.pitch);
    sput_u32(out, static_cast<std::uint32_t>(voice.left));
    sput_u32(out, static_cast<std::uint32_t>(voice.right));
    out.push_back(voice.playing ? 1u : 0u);
    out.push_back(voice.paused ? 1u : 0u);
    sput_u32(out, voice.block);
    sput_u32(out, voice.loop_block);
    sput_u32(out, static_cast<std::uint32_t>(voice.index));
    sput_u32(out, voice.phase);
    sput_u32(out, static_cast<std::uint32_t>(voice.history1));
    sput_u32(out, static_cast<std::uint32_t>(voice.history2));
    for (std::int16_t sample : voice.decoded) sput_i16(out, sample);
    sput_i16(out, voice.previous);
    sput_i16(out, voice.current);
    out.push_back(voice.source_ended ? 1u : 0u);
    out.push_back(voice.primed ? 1u : 0u);
    out.push_back(voice.have_adsr ? 1u : 0u);
    sput_u32(out, voice.adsr1);
    sput_u32(out, voice.adsr2);
    sput_u32(out, static_cast<std::uint32_t>(voice.stage));
    sput_u32(out, static_cast<std::uint32_t>(voice.envelope));
    sput_u32(out, static_cast<std::uint32_t>(voice.envelope_counter));
}

bool sget_voice(SReader &in, SasVoice &voice) {
    voice.address = in.u32();
    voice.size = in.u32();
    const std::uint8_t looping = in.u8();
    const std::uint8_t pcm = in.u8();
    const std::uint8_t noise = in.u8();
    voice.pitch = in.u32();
    voice.left = static_cast<std::int32_t>(in.u32());
    voice.right = static_cast<std::int32_t>(in.u32());
    const std::uint8_t playing = in.u8();
    const std::uint8_t paused = in.u8();
    voice.block = in.u32();
    voice.loop_block = in.u32();
    voice.index = static_cast<std::int32_t>(in.u32());
    voice.phase = in.u32();
    voice.history1 = static_cast<std::int32_t>(in.u32());
    voice.history2 = static_cast<std::int32_t>(in.u32());
    for (std::int16_t &sample : voice.decoded) sample = in.i16();
    voice.previous = in.i16();
    voice.current = in.i16();
    const std::uint8_t source_ended = in.u8();
    const std::uint8_t primed = in.u8();
    const std::uint8_t have_adsr = in.u8();
    voice.adsr1 = in.u32();
    voice.adsr2 = in.u32();
    const std::uint32_t stage = in.u32();
    voice.envelope = static_cast<std::int32_t>(in.u32());
    voice.envelope_counter = static_cast<std::int32_t>(in.u32());
    if (!in.ok || looping > 1u || pcm > 1u || noise > 1u || playing > 1u || paused > 1u || source_ended > 1u ||
        primed > 1u || have_adsr > 1u || stage > static_cast<std::uint32_t>(EnvelopeStage::Release)) {
        return false;
    }
    voice.looping = looping != 0u;
    voice.pcm = pcm != 0u;
    voice.noise = noise != 0u;
    voice.playing = playing != 0u;
    voice.paused = paused != 0u;
    voice.source_ended = source_ended != 0u;
    voice.primed = primed != 0u;
    voice.have_adsr = have_adsr != 0u;
    voice.stage = static_cast<EnvelopeStage>(stage);
    return true;
}

} // namespace

void SasCore::export_snapshot(std::vector<std::uint8_t> &out) const {
    sput_u32(out, grain_);
    sput_u32(out, max_voices_);
    sput_u32(out, output_mode_);
    sput_u32(out, static_cast<std::uint32_t>(reverb_.type));
    sput_u32(out, static_cast<std::uint32_t>(reverb_.delay));
    sput_u32(out, static_cast<std::uint32_t>(reverb_.feedback));
    sput_u32(out, static_cast<std::uint32_t>(reverb_.left));
    sput_u32(out, static_cast<std::uint32_t>(reverb_.right));
    out.push_back(reverb_.dry ? 1u : 0u);
    out.push_back(reverb_.wet ? 1u : 0u);
    for (const SasVoice &voice : voices_) sput_voice(out, voice);
}

bool SasCore::import_snapshot(std::span<const std::uint8_t> bytes, std::string &error) {
    SReader in{bytes.data(), bytes.size()};
    const std::uint32_t grain = in.u32();
    const std::uint32_t max_voices = in.u32();
    const std::uint32_t output_mode = in.u32();
    const std::int32_t type = static_cast<std::int32_t>(in.u32());
    const std::int32_t delay = static_cast<std::int32_t>(in.u32());
    const std::int32_t feedback = static_cast<std::int32_t>(in.u32());
    const std::int32_t left = static_cast<std::int32_t>(in.u32());
    const std::int32_t right = static_cast<std::int32_t>(in.u32());
    const std::uint8_t dry = in.u8();
    const std::uint8_t wet = in.u8();
    if (!in.ok || dry > 1u || wet > 1u || max_voices == 0u || max_voices > kSasMaxVoices) {
        error = "snapshot_bad_length: sas core header malformed";
        return false;
    }
    std::array<SasVoice, kSasMaxVoices> voices{};
    for (std::uint32_t i = 0; i < kSasMaxVoices; ++i) {
        if (!sget_voice(in, voices[i])) {
            error = "snapshot_bad_length: sas voice malformed";
            return false;
        }
    }
    if (!in.ok || in.left != 0u) {
        error = "snapshot_truncated: trailing bytes in sas core record";
        return false;
    }
    grain_ = grain != 0u && grain <= 2048u ? grain : 256u;
    max_voices_ = max_voices;
    output_mode_ = output_mode;
    reverb_.type = type;
    reverb_.delay = delay;
    reverb_.feedback = feedback;
    reverb_.left = left;
    reverb_.right = right;
    reverb_.dry = dry != 0u;
    reverb_.wet = wet != 0u;
    voices_ = voices;
    error.clear();
    return true;
}

bool export_snapshot_sas(std::vector<std::uint8_t> &bytes_out, std::string &error) {
    bytes_out.clear();
    sput_u32(bytes_out, static_cast<std::uint32_t>(g_cores.size()));
    for (const auto &[handle, core] : g_cores) {
        sput_u32(bytes_out, handle);
        std::vector<std::uint8_t> record;
        core.export_snapshot(record);
        sput_u32(bytes_out, static_cast<std::uint32_t>(record.size()));
        bytes_out.insert(bytes_out.end(), record.begin(), record.end());
    }
    error.clear();
    return true;
}

bool import_snapshot_sas(std::span<const std::uint8_t> bytes, std::string &error) {
    SReader in{bytes.data(), bytes.size()};
    const std::uint32_t count = in.u32();
    if (!in.ok || count > 16u) {
        error = "snapshot_truncated: sas core count malformed";
        return false;
    }
    std::map<std::uint32_t, SasCore> staged;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t handle = in.u32();
        const std::uint32_t length = in.u32();
        if (!in.ok || length == 0u || length > 65536u || in.left < length) {
            error = "snapshot_truncated: sas core record truncated";
            return false;
        }
        SasCore core;
        if (!core.import_snapshot(std::span<const std::uint8_t>(in.p, length), error)) return false;
        in.p += length;
        in.left -= length;
        if (!staged.emplace(handle, std::move(core)).second) {
            error = "snapshot_duplicate_uid: duplicate sas handle";
            return false;
        }
    }
    if (!in.ok || in.left != 0u) {
        error = "snapshot_truncated: trailing bytes in sas section";
        return false;
    }
    g_cores = std::move(staged);
    error.clear();
    return true;
}

} // namespace gungnir::audio

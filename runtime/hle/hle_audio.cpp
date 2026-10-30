// sceAudio: the eight mixer channels, one buffer each, played out of guest
// memory at 44.1 kHz. Blocking output parks at most one thread per channel
// until the previous buffer drains; the drain is scheduled on the kernel's
// virtual clock, so pacing stays deterministic and headless runs do not sleep.
#include "hle/hle_audio.hpp"

#include "audio/audio_sink.hpp"
#include "audio/sas_core.hpp"
#include "hle/kernel.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace gungnir::hle {
namespace {

constexpr std::uint32_t kSampleRate = 44'100u;
constexpr std::uint32_t kChannelCount = 8u;
constexpr std::uint32_t kSampleMax = 0xFFC0u;
constexpr std::uint32_t kFormatMono = 0x10u;
constexpr std::uint32_t kMixerBlock = 64u;

constexpr std::uint32_t kNotInit = 0x80260001u;
constexpr std::uint32_t kChannelBusy = 0x80260002u;
constexpr std::uint32_t kInvalidChannel = 0x80260003u;
constexpr std::uint32_t kNoChannels = 0x80260005u;
constexpr std::uint32_t kInvalidSize = 0x80260006u;
constexpr std::uint32_t kInvalidFormat = 0x80260007u;
constexpr std::uint32_t kNotReserved = 0x80260008u;
constexpr std::uint32_t kInvalidVolume = 0x8026000Bu;

struct AudioChannel {
    bool reserved{};
    std::uint32_t sample_count{};
    std::uint32_t format{};
    std::uint32_t left_volume{};
    std::uint32_t right_volume{};
    std::uint32_t sample_address{}; // 0 for a null-buffer drain
    std::uint32_t remaining{};
    std::uint64_t start_us{};
    SceUID waiting{}; // parked blocking caller, at most one
    std::uint32_t waiting_address{};
    std::int32_t waiting_left{};
    std::int32_t waiting_right{};
};

std::array<AudioChannel, kChannelCount> g_channels{};
std::vector<std::int16_t> g_staging;

std::uint32_t remaining_at(const AudioChannel &channel, std::uint64_t now) noexcept {
    if (channel.remaining == 0u) return 0u;
    if (now <= channel.start_us) return channel.remaining;
    const std::uint64_t consumed = (now - channel.start_us) * kSampleRate / 1'000'000u;
    const std::uint64_t blocks = consumed & ~static_cast<std::uint64_t>(kMixerBlock - 1u);
    if (blocks >= channel.remaining) return 0u;
    return channel.remaining - static_cast<std::uint32_t>(blocks);
}

void settle(AudioChannel &channel, std::uint64_t now) noexcept {
    if (channel.remaining != 0u && remaining_at(channel, now) == 0u) {
        channel.sample_address = 0u;
        channel.remaining = 0u;
    }
}

std::int32_t free_channel(std::uint64_t now) {
    for (AudioChannel &channel : g_channels) settle(channel, now);
    for (std::int32_t index = static_cast<std::int32_t>(kChannelCount) - 1; index >= 0; --index) {
        const AudioChannel &channel = g_channels[static_cast<std::size_t>(index)];
        if (channel.sample_count == 0u && channel.sample_address == 0u) return index;
    }
    return -1;
}

void submit_buffer(Runtime &rt, std::size_t index, std::uint32_t address, std::uint64_t now) {
    audio::AudioSink *sink = audio::audio_sink();
    if (sink == nullptr) {
        rt.stop("no audio sink registered");
        return;
    }
    const AudioChannel &channel = g_channels[index];
    const std::uint32_t channels = (channel.format & kFormatMono) != 0u ? 1u : 2u;
    const std::uint32_t frames = channel.sample_count;
    const std::uint32_t bytes = frames * channels * 2u;
    const std::uint8_t *source = rt.memory().raw_pointer(address, bytes);
    if (source == nullptr) {
        rt.stop("sceAudio output buffer is not a mapped guest range");
        return;
    }
    g_staging.resize(static_cast<std::size_t>(frames) * channels);
    for (std::size_t i = 0u; i < g_staging.size(); ++i) {
        const std::uint16_t bits = static_cast<std::uint16_t>(
            source[i * 2u] | (static_cast<std::uint16_t>(source[i * 2u + 1u]) << 8u));
        g_staging[i] = static_cast<std::int16_t>(bits);
    }
    audio::AudioBuffer buffer{};
    buffer.samples = g_staging.data();
    buffer.frames = frames;
    buffer.channels = channels;
    buffer.left_volume = channel.left_volume;
    buffer.right_volume = channel.right_volume;
    buffer.channel = static_cast<std::uint32_t>(index);
    buffer.start_us = now;
    if (!sink->submit(buffer)) rt.stop("audio sink rejected a buffer");
}

std::uint32_t enqueue(Runtime &rt, std::size_t index, std::uint32_t address, std::int32_t left,
    std::int32_t right, std::uint64_t now) {
    AudioChannel &channel = g_channels[index];
    settle(channel, now);
    if (!channel.reserved) return kNotInit;
    if (channel.sample_address != 0u) return kChannelBusy;
    channel.remaining = channel.sample_count;
    if (left >= 0) channel.left_volume = static_cast<std::uint32_t>(left);
    if (right >= 0) channel.right_volume = static_cast<std::uint32_t>(right);
    channel.start_us = now;
    channel.sample_address = address;
    if (address != 0u) submit_buffer(rt, index, address, now);
    return channel.sample_count;
}

std::uint32_t drain_us(std::uint32_t samples) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(samples) * 1'000'000u + kSampleRate - 1u) / kSampleRate);
}

std::uint32_t enqueue_blocking(Runtime &rt, AllegrexContext &ctx, std::size_t index, std::uint32_t address,
    std::int32_t left, std::int32_t right) {
    AudioChannel &channel = g_channels[index];
    const std::uint64_t now = kernel().now_us();
    const std::uint32_t result = enqueue(rt, index, address, left, right, now);
    if (result != kChannelBusy) return result;
    if (channel.waiting != 0) return kChannelBusy;
    if (kernel().in_interrupt()) return error::kIllegalContext;
    if (!kernel().dispatch_enabled()) return error::kCanNotWait;

    channel.waiting = kernel().current_uid();
    channel.waiting_address = address;
    channel.waiting_left = left;
    channel.waiting_right = right;
    const std::uint32_t remaining = remaining_at(channel, now);
    WaitState wait{};
    wait.type = WaitType::Audio;
    wait.object = static_cast<SceUID>(index + 1u);
    wait.deadline_us = now + std::max<std::uint32_t>(drain_us(remaining), 1u);
    wait.host_poll = [&rt, index](bool) -> std::optional<std::uint32_t> {
        AudioChannel &waiting = g_channels[index];
        const std::uint64_t at = kernel().now_us();
        waiting.sample_address = 0u;
        waiting.remaining = 0u;
        waiting.waiting = 0u;
        const std::uint32_t retried =
            enqueue(rt, index, waiting.waiting_address, waiting.waiting_left, waiting.waiting_right, at);
        waiting.waiting_address = 0u;
        return retried;
    };
    wait.cancel_hook = [index]() {
        AudioChannel &waiting = g_channels[index];
        waiting.waiting = 0u;
        waiting.waiting_address = 0u;
        waiting.waiting_left = 0u;
        waiting.waiting_right = 0u;
    };
    kernel().block(ctx, wait, 0u);
    return 0u;
}

void finish(AllegrexContext &ctx, std::uint32_t result) {
    kernel().finish(ctx, result);
}

} // namespace

constexpr std::uint32_t kSasVoices = 32u;
constexpr std::uint32_t kSasCoreBytes = 3616u;
constexpr std::uint32_t kSasBadAddress = 0x80420005u;
constexpr std::uint32_t kSasInvalidGrain = 0x80420001u;
constexpr std::uint32_t kSasInvalidMaxVoices = 0x80420002u;
constexpr std::uint32_t kSasInvalidOutputMode = 0x80420003u;
constexpr std::uint32_t kSasInvalidSampleRate = 0x80420004u;
constexpr std::uint32_t kSasInvalidVoice = 0x80420010u;
constexpr std::uint32_t kSasInvalidPitch = 0x80420012u;
constexpr std::uint32_t kSasInvalidAdsrCurve = 0x80420013u;
constexpr std::uint32_t kSasInvalidParameter = 0x80420014u;
constexpr std::uint32_t kSasInvalidLoop = 0x80420015u;
constexpr std::uint32_t kSasVoicePaused = 0x80420016u;
constexpr std::uint32_t kSasInvalidVolume = 0x80420018u;

std::vector<std::int16_t> g_sas_staging;

bool valid_sas_core(Runtime &rt, std::uint32_t core) {
    return core != 0u && (core & 0x3Fu) == 0u && rt.memory().raw_pointer(core, kSasCoreBytes) != nullptr;
}

bool valid_voice(std::uint32_t voice) {
    return voice < kSasVoices;
}

void write_pcm(Runtime &rt, std::uint32_t address, const std::int16_t *samples, std::size_t count) {
    for (std::size_t i = 0u; i < count; ++i)
        rt.memory().store16(address + static_cast<std::uint32_t>(i * 2u),
            static_cast<std::uint16_t>(samples[i]));
}

void register_sas(HleRegistrar &hle) {
    hle.add("sceSasCore", "__sceSasInit", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t core = arg(ctx, 0);
        if (!valid_sas_core(rt, core)) {
            finish(ctx, kSasBadAddress);
            return;
        }
        const std::uint32_t voices = arg(ctx, 2);
        if (voices == 0u || voices > kSasVoices) {
            finish(ctx, kSasInvalidMaxVoices);
            return;
        }
        const std::uint32_t grain = arg(ctx, 1);
        if (grain < 0x40u || grain > 0x800u || (grain & 0x1Fu) != 0u) {
            finish(ctx, kSasInvalidGrain);
            return;
        }
        const std::uint32_t mode = arg(ctx, 3);
        if (mode > 1u) {
            finish(ctx, kSasInvalidOutputMode);
            return;
        }
        if (arg(ctx, 4) != 44'100u) {
            finish(ctx, kSasInvalidSampleRate);
            return;
        }
        audio::sas_core(core).init(grain, voices, mode);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVoice", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        const std::uint32_t size = arg(ctx, 3);
        if (size == 0u || (size & 0xFu) != 0u) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        const std::uint32_t loop = arg(ctx, 4);
        if (loop > 1u) {
            finish(ctx, kSasInvalidLoop);
            return;
        }
        const std::uint32_t address = arg(ctx, 2);
        if (address != 0u && rt.memory().raw_pointer(address, size) == nullptr) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_voice(voice, address, size, loop != 0u);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVoicePCM", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        const std::int32_t samples = static_cast<std::int32_t>(arg(ctx, 3));
        const std::int32_t loop = static_cast<std::int32_t>(arg(ctx, 4));
        if (samples <= 0) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        if (loop < -1 || loop >= samples) {
            finish(ctx, kSasInvalidLoop);
            return;
        }
        const std::uint32_t address = arg(ctx, 2);
        if (address == 0u || rt.memory().raw_pointer(address, static_cast<std::uint32_t>(samples) * 2u) == nullptr) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_voice_pcm(
            voice, address, static_cast<std::uint32_t>(samples), loop);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetPitch", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        const std::uint32_t pitch = arg(ctx, 2);
        if (pitch == 0u || pitch > 0x4000u) {
            finish(ctx, kSasInvalidPitch);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_pitch(voice, pitch);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetVolume", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        const std::int32_t left = static_cast<std::int32_t>(arg(ctx, 2));
        const std::int32_t right = static_cast<std::int32_t>(arg(ctx, 3));
        const std::int32_t send_left = static_cast<std::int32_t>(arg(ctx, 4));
        const std::int32_t send_right = static_cast<std::int32_t>(arg(ctx, 5));
        const auto over = [](std::int32_t value) { return std::abs(value) > 0x1000; };
        if (over(left) || over(right) || over(send_left) || over(send_right)) {
            finish(ctx, kSasInvalidVolume);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_volume(voice, left, right);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetSimpleADSR", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        const std::uint32_t envelope2 = arg(ctx, 3);
        if (((envelope2 >> 13u) & 1u) != 0u) {
            finish(ctx, kSasInvalidAdsrCurve);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_simple_adsr(voice, arg(ctx, 2), envelope2);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOn", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        audio::SasCore &core = audio::sas_core(arg(ctx, 0));
        if (core.voice_paused(voice) || core.voice_playing(voice)) {
            finish(ctx, kSasVoicePaused);
            return;
        }
        core.key_on(voice);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasSetKeyOff", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        audio::SasCore &core = audio::sas_core(arg(ctx, 0));
        if (core.voice_paused(voice) || !core.voice_playing(voice)) {
            finish(ctx, kSasVoicePaused);
            return;
        }
        core.key_off(voice);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasCore", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t core_address = arg(ctx, 0);
        audio::SasCore &core = audio::sas_core(core_address);
        const std::uint32_t grain = core.grain();
        const std::uint32_t out = arg(ctx, 1);
        if (rt.memory().raw_pointer(out, grain * 4u) == nullptr) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        g_sas_staging.resize(static_cast<std::size_t>(grain) * 2u);
        core.render(rt.memory(), g_sas_staging.data(), grain);
        write_pcm(rt, out, g_sas_staging.data(), g_sas_staging.size());
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasCoreWithMix", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t core_address = arg(ctx, 0);
        audio::SasCore &core = audio::sas_core(core_address);
        const std::uint32_t grain = core.grain();
        const std::uint32_t address = arg(ctx, 1);
        if (rt.memory().raw_pointer(address, grain * 4u) == nullptr) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        const auto gain = [](std::int32_t volume) {
            return static_cast<std::int32_t>(std::clamp(volume, 0, 0x1000));
        };
        const std::int32_t left_gain = gain(static_cast<std::int32_t>(arg(ctx, 2)));
        const std::int32_t right_gain = gain(static_cast<std::int32_t>(arg(ctx, 3)));
        g_sas_staging.resize(static_cast<std::size_t>(grain) * 2u);
        core.render(rt.memory(), g_sas_staging.data(), grain);
        for (std::uint32_t i = 0u; i < grain * 2u; ++i) {
            const std::uint16_t bits = rt.memory().load16(address + i * 2u);
            const std::int32_t existing = static_cast<std::int16_t>(bits);
            const std::int32_t value = (g_sas_staging[i] * (i % 2u == 0u ? left_gain : right_gain)) >> 12;
            const std::int32_t mixed = std::clamp(existing + value, -32768, 32767);
            rt.memory().store16(address + i * 2u, static_cast<std::uint16_t>(mixed));
        }
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasGetEndFlag", [](Runtime &, AllegrexContext &ctx) {
        finish(ctx, audio::sas_core(arg(ctx, 0)).end_flag());
    });
    hle.add("sceSasCore", "__sceSasGetEnvelopeHeight", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t voice = arg(ctx, 1);
        if (!valid_voice(voice)) {
            finish(ctx, kSasInvalidVoice);
            return;
        }
        finish(ctx, static_cast<std::uint32_t>(audio::sas_core(arg(ctx, 0)).envelope_height(voice)));
    });
    hle.add("sceSasCore", "__sceSasGetAllEnvelopeHeights", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t address = arg(ctx, 1);
        if (rt.memory().raw_pointer(address, kSasVoices * 4u) == nullptr) {
            finish(ctx, kSasInvalidParameter);
            return;
        }
        audio::SasCore &core = audio::sas_core(arg(ctx, 0));
        for (std::uint32_t voice = 0u; voice < kSasVoices; ++voice)
            rt.memory().store32(address + voice * 4u,
                static_cast<std::uint32_t>(core.envelope_height(voice)));
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasGetPauseFlag", [](Runtime &, AllegrexContext &ctx) {
        finish(ctx, audio::sas_core(arg(ctx, 0)).pause_flag());
    });
    hle.add("sceSasCore", "__sceSasSetPause", [](Runtime &, AllegrexContext &ctx) {
        audio::sas_core(arg(ctx, 0)).set_pause(arg(ctx, 1), arg(ctx, 2) != 0u);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasGetGrain", [](Runtime &, AllegrexContext &ctx) {
        finish(ctx, audio::sas_core(arg(ctx, 0)).grain());
    });
    hle.add("sceSasCore", "__sceSasSetGrain", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t grain = arg(ctx, 1);
        if (grain < 0x40u || grain > 0x800u || (grain & 0x1Fu) != 0u) {
            finish(ctx, kSasInvalidGrain);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_grain(grain);
        finish(ctx, 0u);
    });
    hle.add("sceSasCore", "__sceSasGetOutputmode", [](Runtime &, AllegrexContext &ctx) {
        finish(ctx, audio::sas_core(arg(ctx, 0)).output_mode());
    });
    hle.add("sceSasCore", "__sceSasSetOutputmode", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t mode = arg(ctx, 1);
        if (mode > 1u) {
            finish(ctx, kSasInvalidOutputMode);
            return;
        }
        audio::sas_core(arg(ctx, 0)).set_output_mode(mode);
        finish(ctx, 0u);
    });
}

void reset_audio_state() {
    for (AudioChannel &channel : g_channels) channel = AudioChannel{};
    g_staging.clear();
    g_sas_staging.clear();
    audio::reset_sas_cores();
}

void register_audio(HleRegistrar &hle) {
    hle.add("sceAudio", "sceAudioChReserve", [](Runtime &, AllegrexContext &ctx) {
        std::int32_t channel = static_cast<std::int32_t>(arg(ctx, 0));
        const std::uint64_t now = kernel().now_us();
        if (channel < 0) {
            channel = free_channel(now);
            if (channel < 0) {
                finish(ctx, kNoChannels);
                return;
            }
        }
        if (channel >= static_cast<std::int32_t>(kChannelCount)) {
            finish(ctx, kInvalidChannel);
            return;
        }
        AudioChannel &state = g_channels[static_cast<std::size_t>(channel)];
        if (state.reserved) {
            finish(ctx, kInvalidChannel);
            return;
        }
        const std::uint32_t samples = arg(ctx, 1);
        const std::uint32_t format = arg(ctx, 2);
        if (samples == 0u || (samples & (kMixerBlock - 1u)) != 0u || samples > kSampleMax) {
            finish(ctx, kInvalidSize);
            return;
        }
        if (format != 0u && format != kFormatMono) {
            finish(ctx, kInvalidFormat);
            return;
        }
        state = AudioChannel{};
        state.reserved = true;
        state.sample_count = samples;
        state.format = format;
        finish(ctx, static_cast<std::uint32_t>(channel));
    });
    hle.add("sceAudio", "sceAudioChRelease", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        AudioChannel &state = g_channels[channel];
        if (!state.reserved) {
            finish(ctx, kNotReserved);
            return;
        }
        if (state.waiting != 0) {
            finish(ctx, kChannelBusy);
            return;
        }
        // Only the reservation goes away; a handed-over buffer plays out.
        state.reserved = false;
        state.sample_count = 0u;
        finish(ctx, 0u);
    });
    const auto output = [](bool blocking, bool panned) {
        return [blocking, panned](Runtime &rt, AllegrexContext &ctx) {
            const std::int32_t left = static_cast<std::int32_t>(arg(ctx, 1));
            const std::int32_t right =
                panned ? static_cast<std::int32_t>(arg(ctx, 2)) : left;
            const std::uint32_t address = arg(ctx, panned ? 3u : 2u);
            const bool bad_volume = blocking && panned
                ? (static_cast<std::uint32_t>(left | right) > 0xFFFFu)
                : (left > 0xFFFF || right > 0xFFFF);
            if (bad_volume) {
                finish(ctx, kInvalidVolume);
                return;
            }
            const std::uint32_t channel = arg(ctx, 0);
            if (channel >= kChannelCount) {
                finish(ctx, kInvalidChannel);
                return;
            }
            if (blocking) {
                kernel().finish(ctx, enqueue_blocking(rt, ctx, channel, address, left, right));
                return;
            }
            finish(ctx, enqueue(rt, channel, address, left, right, kernel().now_us()));
        };
    };
    hle.add("sceAudio", "sceAudioOutput", output(false, false));
    hle.add("sceAudio", "sceAudioOutputBlocking", output(true, false));
    hle.add("sceAudio", "sceAudioOutputPanned", output(false, true));
    hle.add("sceAudio", "sceAudioOutputPannedBlocking", output(true, true));
    hle.add("sceAudio", "sceAudioGetChannelRestLen", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        const std::uint64_t now = kernel().now_us();
        AudioChannel &state = g_channels[channel];
        settle(state, now);
        std::uint32_t rest = remaining_at(state, now);
        if (state.waiting != 0) rest += state.sample_count;
        finish(ctx, rest);
    });
    hle.add("sceAudio", "sceAudioGetChannelRestLength", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        const std::uint64_t now = kernel().now_us();
        AudioChannel &state = g_channels[channel];
        settle(state, now);
        std::uint32_t rest = state.sample_address != 0u ? remaining_at(state, now) : 0u;
        if (state.waiting != 0) rest += state.sample_count;
        finish(ctx, rest);
    });
    hle.add("sceAudio", "sceAudioSetChannelDataLen", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        const std::uint32_t samples = arg(ctx, 1);
        if (samples == 0u || (samples & (kMixerBlock - 1u)) != 0u || samples > kSampleMax) {
            finish(ctx, kInvalidSize);
            return;
        }
        AudioChannel &state = g_channels[channel];
        if (state.waiting != 0) {
            finish(ctx, kChannelBusy);
            return;
        }
        if (!state.reserved) {
            finish(ctx, kNotInit);
            return;
        }
        state.sample_count = samples;
        finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelConfig", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        AudioChannel &state = g_channels[channel];
        settle(state, kernel().now_us());
        if (state.waiting != 0 || state.sample_address != 0u) {
            finish(ctx, kChannelBusy);
            return;
        }
        if (!state.reserved) {
            finish(ctx, kNotReserved);
            return;
        }
        const std::uint32_t format = arg(ctx, 1);
        if (format != 0u && format != kFormatMono) {
            finish(ctx, kInvalidFormat);
            return;
        }
        state.format = format;
        finish(ctx, 0u);
    });
    hle.add("sceAudio", "sceAudioChangeChannelVolume", [](Runtime &, AllegrexContext &ctx) {
        const std::int32_t left = static_cast<std::int32_t>(arg(ctx, 1));
        const std::int32_t right = static_cast<std::int32_t>(arg(ctx, 2));
        if (left > 0xFFFF || right > 0xFFFF) {
            finish(ctx, kInvalidVolume);
            return;
        }
        const std::uint32_t channel = arg(ctx, 0);
        if (channel >= kChannelCount) {
            finish(ctx, kInvalidChannel);
            return;
        }
        AudioChannel &state = g_channels[channel];
        if (left >= 0) state.left_volume = static_cast<std::uint32_t>(left);
        if (right >= 0) state.right_volume = static_cast<std::uint32_t>(right);
        finish(ctx, 0u);
    });
    register_sas(hle);
}

} // namespace gungnir::hle

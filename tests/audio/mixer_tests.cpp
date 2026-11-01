#include "audio/audio_mixer.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

using gungnir::audio::AudioBuffer;
using gungnir::audio::MixerSink;

namespace {

int failures = 0;

void check(bool condition, std::string_view what) {
    if (condition) return;
    std::cerr << "audio_mixer_tests: FAIL: " << what << "\n";
    ++failures;
}

AudioBuffer buffer(const std::vector<std::int16_t> &samples, std::uint32_t channels,
    std::uint32_t left = 0x8000u, std::uint32_t right = 0x8000u, std::uint32_t channel = 0u) {
    AudioBuffer value{};
    value.samples = samples.data();
    value.frames = static_cast<std::uint32_t>(samples.size() / channels);
    value.channels = channels;
    value.left_volume = left;
    value.right_volume = right;
    value.channel = channel;
    return value;
}

void mixing_contracts() {
    MixerSink sink(16u, 0u);
    const std::vector<std::int16_t> stereo = {100, -100, 200, -200, 300, -300, 400, -400};
    check(sink.submit(buffer(stereo, 2u)), "stereo buffer accepted");
    check(sink.available_frames() == 4u, "queued frames are visible to a producer");
    std::array<std::int16_t, 8> output{};
    check(sink.pull(output) == 4u, "pull serves the requested frames");
    check(output[0] == 100 && output[1] == -100 && output[7] == -400, "stereo bytes pass through at unity");
    check(sink.stats().underrun_frames == 0u, "no underrun when data is queued");
    check(sink.stats().peak == 400u, "peak describes the mixed output");

    check(sink.submit(buffer(stereo, 2u, 0x4000u, 0x2000u)), "attenuated buffer accepted");
    output.fill(0);
    sink.pull(output);
    check(output[0] == 50 && output[1] == -25, "left and right volumes apply independently");

    const std::vector<std::int16_t> mono = {1000, -1000};
    check(sink.submit(buffer(mono, 1u, 0x8000u, 0x4000u)), "mono buffer accepted");
    output.fill(0);
    sink.pull(output);
    check(output[0] == 1000 && output[1] == 500, "mono samples feed both channels with their volumes");

    const std::vector<std::int16_t> clipped = {32767, -32768};
    check(sink.submit(buffer(clipped, 2u)), "clipping buffer accepted");
    output.fill(0);
    sink.pull(output);
    check(output[0] == 32767 && output[1] == -32768, "full-scale samples stay in range");

    sink.reset();
    check(sink.submit(buffer(std::vector<std::int16_t>{1, 2}, 2u)), "short buffer accepted");
    output.fill(99);
    check(sink.pull(output) == 4u, "short pull still serves the whole device request");
    check(output[0] == 1 && output[1] == 2, "available samples come first");
    check(output[2] == 0 && output[7] == 0, "missing samples are silence");
    check(sink.stats().underrun_frames == 3u, "missing frames are counted as underrun");
}

void channels_mix_instead_of_queueing() {
    MixerSink sink(64u, 0u);
    const std::vector<std::int16_t> music = {1000, -1000, 2000, -2000};
    const std::vector<std::int16_t> effect = {500, 500, 250, 250};
    check(sink.submit(buffer(music, 2u, 0x8000u, 0x8000u, 5u)), "music channel accepted");
    check(sink.submit(buffer(effect, 2u, 0x8000u, 0x8000u, 6u)), "effect channel accepted");
    std::array<std::int16_t, 4> output{};
    sink.pull(output);
    check(output[0] == 1500 && output[1] == -500 && output[2] == 2250 && output[3] == -1750,
        "two channels are summed on one timeline");
    check(sink.available_frames() == 0u, "mixed channels share the same timeline");
}

void overflow_restarts_the_timeline() {
    MixerSink bounded(4u, 0u);
    const std::vector<std::int16_t> four = {1, 1, 2, 2, 3, 3, 4, 4};
    check(bounded.submit(buffer(four, 2u)), "full ring accepted");
    const std::vector<std::int16_t> two = {5, 5, 6, 6};
    check(bounded.submit(buffer(two, 2u)), "overflow does not reject the submission");
    check(bounded.stats().overruns == 1u, "overflow is counted");
    check(bounded.stats().rejected_buffers == 0u, "overflow is not a rejected buffer");
    std::array<std::int16_t, 4> output{};
    bounded.pull(output);
    check(output[0] == 5 && output[2] == 6, "the timeline restarts with the newest buffer");
}

void late_channel_resyncs() {
    MixerSink sink(256u, 4u);
    const std::vector<std::int16_t> four = {1, 1, 2, 2, 3, 3, 4, 4};
    check(sink.submit(buffer(four, 2u)), "first buffer accepted");
    std::array<std::int16_t, 200> output{};
    sink.pull(output);
    check(sink.submit(buffer(four, 2u)), "late buffer accepted");
    check(sink.stats().resyncs == 1u, "a channel that fell behind the device resyncs");
    check(sink.available_frames() == 8u, "resynced audio starts one lead ahead of the device");
    std::array<std::int16_t, 16> tail{};
    sink.pull(tail);
    check(tail[0] == 0 && tail[8] == 1 && tail[12] == 3,
        "a resynced channel never lands in the past");
}

} // namespace

int main() {
    mixing_contracts();
    channels_mix_instead_of_queueing();
    overflow_restarts_the_timeline();
    late_channel_resyncs();
    if (failures != 0) {
        std::cerr << "audio_mixer_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio_mixer_tests: OK\n";
    return 0;
}

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
    std::uint32_t left = 0x8000u, std::uint32_t right = 0x8000u) {
    AudioBuffer value{};
    value.samples = samples.data();
    value.frames = static_cast<std::uint32_t>(samples.size() / channels);
    value.channels = channels;
    value.left_volume = left;
    value.right_volume = right;
    return value;
}

void mixing_contracts() {
    MixerSink sink(16u);
    const std::vector<std::int16_t> stereo = {100, -100, 200, -200, 300, -300, 400, -400};
    check(sink.submit(buffer(stereo, 2u)), "stereo buffer accepted");
    std::array<std::int16_t, 8> output{};
    check(sink.pull(output) == 4u, "pull serves the requested frames");
    check(output[0] == 100 && output[1] == -100 && output[7] == -400, "stereo bytes pass through at unity");
    check(sink.stats().underrun_frames == 0u, "no underrun when data is queued");

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

    MixerSink bounded(4u);
    const std::vector<std::int16_t> four = {1, 1, 2, 2, 3, 3, 4, 4};
    check(bounded.submit(buffer(four, 2u)), "full ring accepted");
    check(!bounded.submit(buffer(std::vector<std::int16_t>{5, 5}, 2u)), "overflow is rejected");
    check(bounded.stats().overruns == 1u, "overflow is counted");
    output.fill(0);
    bounded.pull(output);
    check(output[0] == 1 && output[6] == 4, "overflow leaves the queued data intact");
}

} // namespace

int main() {
    mixing_contracts();
    if (failures != 0) {
        std::cerr << "audio_mixer_tests: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio_mixer_tests: OK\n";
    return 0;
}

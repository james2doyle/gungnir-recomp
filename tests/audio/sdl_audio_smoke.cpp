#include "audio/audio_mixer.hpp"
#include "platform/sdl_audio.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::cerr << "sdl_audio_unavailable: " << SDL_GetError() << "\n";
        return 77;
    }
    gungnir::audio::MixerSink mixer(44'100u);
    std::string error;
    auto device = gungnir::audio::SdlAudio::open(mixer, error);
    if (!device) {
        std::cerr << "sdl_audio_unavailable: " << error << "\n";
        SDL_Quit();
        return 77;
    }

    constexpr std::uint32_t kFrames = 256u;
    constexpr std::uint32_t kBlocks = 40u;
    constexpr double kTwoPi = 6.28318530717958647692;
    std::vector<std::int16_t> samples(kFrames * 2u);
    double phase = 0.0;
    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
            const auto value = static_cast<std::int16_t>(8000.0 * std::sin(phase));
            samples[frame * 2u] = value;
            samples[frame * 2u + 1u] = value;
            phase += kTwoPi * 440.0 / 44'100.0;
            if (phase > kTwoPi) phase -= kTwoPi;
        }
        gungnir::audio::AudioBuffer buffer{};
        buffer.samples = samples.data();
        buffer.frames = kFrames;
        buffer.channels = 2u;
        buffer.left_volume = 0x8000u;
        buffer.right_volume = 0x8000u;
        if (!mixer.submit(buffer)) {
            std::cerr << "sdl_audio_smoke: mixer overflow\n";
            return 1;
        }
    }
    SDL_Delay(250);
    const auto stats = mixer.stats();
    if (stats.frames_pulled == 0u) {
        std::cerr << "sdl_audio_smoke: device pulled no frames\n";
        return 1;
    }
    std::cout << "sdl_audio_smoke: OK pulled=" << stats.frames_pulled
              << " underrun=" << stats.underrun_frames << "\n";
    device.reset();
    SDL_Quit();
    return 0;
}

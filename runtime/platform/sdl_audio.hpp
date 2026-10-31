#pragma once

#include <SDL3/SDL_audio.h>

#include <memory>
#include <string>

namespace gungnir::audio {

class MixerSink;

class SdlAudio {
public:
    static std::unique_ptr<SdlAudio> open(MixerSink &mixer, std::string &error);
    ~SdlAudio();

    SdlAudio(const SdlAudio &) = delete;
    SdlAudio &operator=(const SdlAudio &) = delete;

    bool pause(std::string &error);
    bool resume(std::string &error);

private:
    struct Impl;
    explicit SdlAudio(std::unique_ptr<Impl> impl);
    static void audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int total);
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::audio

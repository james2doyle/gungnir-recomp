#include "platform/sdl_audio.hpp"

#include "audio/audio_mixer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace gungnir::audio {

struct SdlAudio::Impl {
    MixerSink *mixer{};
    SDL_AudioStream *stream{};
    std::vector<std::int16_t> buffer;
};

void SdlAudio::audio_callback(void *userdata, SDL_AudioStream *stream, int additional, int) {
    auto *impl = static_cast<Impl *>(userdata);
    if (impl == nullptr || stream == nullptr || additional <= 0) return;
    const std::size_t frames = static_cast<std::size_t>(additional) / (2u * sizeof(std::int16_t));
    impl->buffer.assign(frames * 2u, 0);
    impl->mixer->pull(impl->buffer);
    const int bytes = static_cast<int>(frames * 2u * sizeof(std::int16_t));
    SDL_PutAudioStreamData(stream, impl->buffer.data(), bytes);
    if (bytes < additional) {
        static const std::vector<std::uint8_t> silence(16u, 0u);
        int remaining = additional - bytes;
        while (remaining > 0) {
            const int chunk = std::min<int>(remaining, static_cast<int>(silence.size()));
            SDL_PutAudioStreamData(stream, silence.data(), chunk);
            remaining -= chunk;
        }
    }
}

SdlAudio::SdlAudio(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SdlAudio::~SdlAudio() {
    if (impl_ && impl_->stream != nullptr) SDL_DestroyAudioStream(impl_->stream);
}

std::unique_ptr<SdlAudio> SdlAudio::open(MixerSink &mixer, std::string &error) {
    error.clear();
    auto impl = std::make_unique<Impl>();
    impl->mixer = &mixer;
    const SDL_AudioSpec specification{SDL_AUDIO_S16LE, 2, 44'100};
    impl->stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &specification, audio_callback, impl.get());
    if (impl->stream == nullptr) {
        error = SDL_GetError();
        return nullptr;
    }
    if (!SDL_ResumeAudioStreamDevice(impl->stream)) {
        error = SDL_GetError();
        SDL_DestroyAudioStream(impl->stream);
        impl->stream = nullptr;
        return nullptr;
    }
    return std::unique_ptr<SdlAudio>(new SdlAudio(std::move(impl)));
}

bool SdlAudio::pause(std::string &error) {
    error.clear();
    if (!impl_ || impl_->stream == nullptr) {
        error = "sdl_audio_closed";
        return false;
    }
    if (!SDL_PauseAudioStreamDevice(impl_->stream)) {
        error = SDL_GetError();
        return false;
    }
    return true;
}

bool SdlAudio::resume(std::string &error) {
    error.clear();
    if (!impl_ || impl_->stream == nullptr) {
        error = "sdl_audio_closed";
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(impl_->stream)) {
        error = SDL_GetError();
        return false;
    }
    return true;
}

} // namespace gungnir::audio

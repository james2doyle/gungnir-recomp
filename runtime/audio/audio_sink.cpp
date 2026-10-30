#include "audio/audio_sink.hpp"

namespace gungnir::audio {
namespace {

AudioSink *g_sink{};

} // namespace

void set_audio_sink(AudioSink *sink) {
    g_sink = sink;
}

AudioSink *audio_sink() {
    return g_sink;
}

} // namespace gungnir::audio

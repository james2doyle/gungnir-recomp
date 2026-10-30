#pragma once

#include "hle/hle_common.hpp"

namespace gungnir::hle {

// sceAudio output channels (T6f boot discoveries). The device backend is
// supplied by the host through audio::AudioSink; without one, handing a buffer
// to a channel stops the runtime with a named error.
void register_audio(HleRegistrar &hle);
void reset_audio_state();

} // namespace gungnir::hle

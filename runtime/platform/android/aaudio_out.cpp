#include "platform/android/aaudio_out.hpp"

#include "audio/audio_mixer.hpp"

#include <android/log.h>

#include <atomic>
#include <span>

namespace gungnir::audio {

struct AAudioOut::Impl {
    MixerSink *mixer{};
    AAudioStream *stream{};
    std::atomic<std::int32_t> error_code{AAUDIO_OK};

    static aaudio_data_callback_result_t data_callback(
        AAudioStream *, void *userdata, void *audio_data, std::int32_t frames) {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl == nullptr || impl->mixer == nullptr || audio_data == nullptr || frames <= 0)
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        impl->mixer->pull(std::span<std::int16_t>(
            static_cast<std::int16_t *>(audio_data), static_cast<std::size_t>(frames) * 2u));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void error_callback(AAudioStream *, void *userdata, aaudio_result_t error) {
        auto *impl = static_cast<Impl *>(userdata);
        if (impl != nullptr) impl->error_code.store(error);
        __android_log_print(ANDROID_LOG_ERROR, "gungnir", "aaudio_error: %d", error);
    }
};

namespace {

std::string describe(aaudio_result_t result) {
    return std::string(AAudio_convertResultToText(result));
}

} // namespace

AAudioOut::AAudioOut(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

AAudioOut::~AAudioOut() {
    if (!impl_ || impl_->stream == nullptr) return;
    AAudioStream_requestStop(impl_->stream);
    AAudioStream_close(impl_->stream);
}

std::unique_ptr<AAudioOut> AAudioOut::open(MixerSink &mixer, std::string &error) {
    error.clear();
    auto impl = std::make_unique<Impl>();
    impl->mixer = &mixer;

    AAudioStreamBuilder *builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        error = "aaudio_builder_failed: " + describe(result);
        return nullptr;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSampleRate(builder, 44'100);
    AAudioStreamBuilder_setDataCallback(builder, &Impl::data_callback, impl.get());
    AAudioStreamBuilder_setErrorCallback(builder, &Impl::error_callback, impl.get());

    result = AAudioStreamBuilder_openStream(builder, &impl->stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || impl->stream == nullptr) {
        error = "aaudio_open_failed: " + describe(result);
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "gungnir",
        "aaudio_stream rate=%d channels=%d burst=%d capacity=%d frames=%d perf=%d",
        AAudioStream_getSampleRate(impl->stream), AAudioStream_getChannelCount(impl->stream),
        AAudioStream_getFramesPerBurst(impl->stream), AAudioStream_getBufferCapacityInFrames(impl->stream),
        AAudioStream_getBufferSizeInFrames(impl->stream), AAudioStream_getPerformanceMode(impl->stream));
    result = AAudioStream_requestStart(impl->stream);
    if (result != AAUDIO_OK) {
        error = "aaudio_start_failed: " + describe(result);
        AAudioStream_close(impl->stream);
        impl->stream = nullptr;
        return nullptr;
    }
    return std::unique_ptr<AAudioOut>(new AAudioOut(std::move(impl)));
}

bool AAudioOut::pause(std::string &error) {
    error.clear();
    if (!impl_ || impl_->stream == nullptr) {
        error = "aaudio_closed";
        return false;
    }
    const aaudio_result_t result = AAudioStream_requestPause(impl_->stream);
    if (result != AAUDIO_OK) {
        error = "aaudio_pause_failed: " + describe(result);
        return false;
    }
    return true;
}

bool AAudioOut::resume(std::string &error) {
    error.clear();
    if (!impl_ || impl_->stream == nullptr) {
        error = "aaudio_closed";
        return false;
    }
    const aaudio_result_t result = AAudioStream_requestStart(impl_->stream);
    if (result != AAUDIO_OK) {
        error = "aaudio_resume_failed: " + describe(result);
        return false;
    }
    return true;
}

std::int32_t AAudioOut::xruns() const {
    if (!impl_ || impl_->stream == nullptr) return 0;
    return AAudioStream_getXRunCount(impl_->stream);
}

} // namespace gungnir::audio

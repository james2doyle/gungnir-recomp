#pragma once

#include <aaudio/AAudio.h>

#include <cstdint>
#include <memory>
#include <string>

namespace gungnir::audio {

class MixerSink;

class AAudioOut {
public:
    static std::unique_ptr<AAudioOut> open(MixerSink &mixer, std::string &error);
    ~AAudioOut();

    AAudioOut(const AAudioOut &) = delete;
    AAudioOut &operator=(const AAudioOut &) = delete;

    bool pause(std::string &error);
    bool resume(std::string &error);
    [[nodiscard]] std::int32_t xruns() const;

private:
    struct Impl;
    explicit AAudioOut(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace gungnir::audio

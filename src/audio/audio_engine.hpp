#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "domain/models.hpp"

namespace resona {

class AudioEngine {
public:
    using Callback = std::function<void()>;
    using ErrorCallback = std::function<void(const std::string&)>;

    AudioEngine();
    ~AudioEngine();

    void play(const Track& track, const StreamSource& stream = {});
    void stop();
    void setPaused(bool paused);
    void togglePause();
    void seekMs(int64_t ms);
    void setVolume(float volume);
    void reportError(const std::string& message);

    PlayerSnapshot snapshot() const;
    void setEndCallback(Callback callback);
    void setErrorCallback(ErrorCallback callback);
    void wait();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace resona

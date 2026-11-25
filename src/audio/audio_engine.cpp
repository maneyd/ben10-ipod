#include "audio/audio_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <pulse/error.h>
#include <pulse/simple.h>
}

#include "network/http.hpp"

namespace resona {

namespace {

constexpr int64_t kRangeSize = 900000;
constexpr int kIoBufferSize = 32768;

class RemoteReader {
public:
    RemoteReader(Http& http, std::string url, std::string userAgent)
        : http_(http), url_(std::move(url)), userAgent_(std::move(userAgent)) {}

    int read(uint8_t* buffer, int bufferSize) {
        if (cursor_ >= size_ && size_ >= 0) return AVERROR_EOF;
        if (cursor_ < start_ || cursor_ >= start_ + static_cast<int64_t>(data_.size())) {
            if (!fetch(cursor_)) return AVERROR(EIO);
        }
        const auto offset = static_cast<size_t>(cursor_ - start_);
        if (offset >= data_.size()) return AVERROR_EOF;
        const size_t amount = std::min<size_t>(bufferSize, data_.size() - offset);
        std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset), amount, buffer);
        cursor_ += static_cast<int64_t>(amount);
        return static_cast<int>(amount);
    }

    int64_t seek(int64_t offset, int whence) {
        if ((whence & AVSEEK_SIZE) != 0) return size_;
        if (whence == SEEK_SET) cursor_ = offset;
        else if (whence == SEEK_CUR) cursor_ += offset;
        else if (whence == SEEK_END) cursor_ = size_ + offset;
        else return AVERROR(EINVAL);
        if (cursor_ < 0) return AVERROR(EINVAL);
        return cursor_;
    }

private:
    bool fetch(int64_t start) {
        const std::vector<std::pair<std::string, std::string>> headers = {
            {"Origin", "https://www.youtube.com"},
            {"Referer", "https://www.youtube.com/"},
            {"Accept", "*/*"},
        };
        const auto result = http_.range(url_, start, kRangeSize, headers, userAgent_);
        if (result.status == 416) return false;
        if (result.status < 200 || result.status >= 300) return false;
        if (result.body.empty()) return false;
        data_.assign(result.body.begin(), result.body.end());
        start_ = start;
        if (result.total >= 0) size_ = result.total;
        return true;
    }

    Http& http_;
    std::string url_;
    std::string userAgent_;
    std::vector<uint8_t> data_;
    int64_t start_ = 0;
    int64_t cursor_ = 0;
    int64_t size_ = -1;
};

int avioRead(void* opaque, uint8_t* buffer, int bufferSize) {
    return static_cast<RemoteReader*>(opaque)->read(buffer, bufferSize);
}

int64_t avioSeek(void* opaque, int64_t offset, int whence) {
    return static_cast<RemoteReader*>(opaque)->seek(offset, whence);
}

}  // namespace

struct AudioEngine::Impl {
    Http http;
    std::mutex stateMutex;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};
    std::atomic<int64_t> positionMs{0};
    std::atomic<int64_t> durationMs{0};
    std::atomic<int64_t> pendingSeek{-1};
    std::atomic<float> volume{1.0f};
    Track current;
    StreamSource stream;
    std::string error;
    Callback endCallback;
    ErrorCallback errorCallback;

    void setError(const std::string& message) {
        std::lock_guard<std::mutex> lock(stateMutex);
        error = message;
    }

    void run() {
        AVFormatContext* format = nullptr;
        AVCodecContext* decoder = nullptr;
        SwrContext* resampler = nullptr;
        pa_simple* pulse = nullptr;
        AVIOContext* avio = nullptr;
        std::unique_ptr<RemoteReader> remote;
        int audioStream = -1;
        bool naturalEnd = false;
        int64_t discardUntilMs = -1;

        auto cleanup = [&] {
            if (decoder != nullptr) avcodec_free_context(&decoder);
            if (resampler != nullptr) swr_free(&resampler);
            if (pulse != nullptr) pa_simple_free(pulse);
            if (format != nullptr) avformat_close_input(&format);
            if (avio != nullptr) {
                av_freep(&avio->buffer);
                avio_context_free(&avio);
            }
        };

        try {
            const bool useRemote = current.source == TrackSource::YouTube && !stream.url.empty();
            std::string sourceName;
            if (useRemote) {
                remote = std::make_unique<RemoteReader>(http, stream.url, stream.userAgent);
                avio = avio_alloc_context(static_cast<unsigned char*>(av_malloc(kIoBufferSize)), kIoBufferSize, 0,
                                          remote.get(), avioRead, nullptr, avioSeek);
                if (avio == nullptr) throw std::runtime_error("Could not allocate stream reader");
                format = avformat_alloc_context();
                format->pb = avio;
                format->flags |= AVFMT_FLAG_CUSTOM_IO;
                sourceName = stream.url;
            } else {
                sourceName = current.location;
            }

            if (avformat_open_input(&format, useRemote ? nullptr : sourceName.c_str(), nullptr, nullptr) < 0) {
                throw std::runtime_error("Could not open audio source");
            }
            if (avformat_find_stream_info(format, nullptr) < 0) {
                throw std::runtime_error("Could not read audio stream information");
            }
            const AVCodec* codec = nullptr;
            audioStream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
            if (audioStream < 0 || codec == nullptr) throw std::runtime_error("No playable audio stream found");

            decoder = avcodec_alloc_context3(codec);
            if (decoder == nullptr) throw std::runtime_error("Could not allocate decoder");
            if (avcodec_parameters_to_context(decoder, format->streams[audioStream]->codecpar) < 0) {
                throw std::runtime_error("Could not configure decoder");
            }
            if (avcodec_open2(decoder, codec, nullptr) < 0) throw std::runtime_error("Could not open decoder");

            const int sampleRate = decoder->sample_rate > 0 ? decoder->sample_rate : 44100;
            AVChannelLayout outputLayout;
            av_channel_layout_default(&outputLayout, 2);
            if (swr_alloc_set_opts2(&resampler, &outputLayout, AV_SAMPLE_FMT_S16, sampleRate,
                                    &decoder->ch_layout, decoder->sample_fmt, sampleRate, 0, nullptr) < 0) {
                throw std::runtime_error("Could not configure audio converter");
            }
            if (swr_init(resampler) < 0) throw std::runtime_error("Could not initialize audio converter");

            pa_sample_spec sampleSpec{PA_SAMPLE_S16LE, static_cast<uint32_t>(sampleRate), 2};
            int paError = 0;
            pulse = pa_simple_new(nullptr, "resona-player", PA_STREAM_PLAYBACK, nullptr, "playback", &sampleSpec,
                                  nullptr, nullptr, &paError);
            if (pulse == nullptr) {
                throw std::runtime_error(std::string("Could not open audio output: ") + pa_strerror(paError));
            }

            if (format->duration > 0) {
                durationMs.store(av_rescale_q(format->duration, AV_TIME_BASE_Q, AVRational{1, 1000}));
            } else if (format->streams[audioStream]->duration > 0) {
                durationMs.store(av_rescale_q(format->streams[audioStream]->duration,
                                              format->streams[audioStream]->time_base, AVRational{1, 1000}));
            }

            AVPacket* packet = av_packet_alloc();
            AVFrame* frame = av_frame_alloc();
            if (packet == nullptr || frame == nullptr) {
                av_packet_free(&packet);
                av_frame_free(&frame);
                throw std::runtime_error("Could not allocate decoding buffers");
            }

            while (running.load()) {
                const int64_t seekTarget = pendingSeek.exchange(-1);
                if (seekTarget >= 0) {
                    const int64_t targetTs = av_rescale_q(seekTarget, AVRational{1, 1000},
                                                          format->streams[audioStream]->time_base);
                    int seekResult = av_seek_frame(format, audioStream, targetTs, AVSEEK_FLAG_BACKWARD);
                    if (seekResult < 0) {
                        seekResult = avformat_seek_file(format, audioStream, std::numeric_limits<int64_t>::min(),
                                                        targetTs, std::numeric_limits<int64_t>::max(), 0);
                    }
                    if (seekResult >= 0) {
                        avcodec_flush_buffers(decoder);
                        swr_close(resampler);
                        swr_init(resampler);
                        if (pulse != nullptr) pa_simple_flush(pulse, &paError);
                        positionMs.store(seekTarget);
                        discardUntilMs = seekTarget;
                    }
                }
                if (paused.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                const int readResult = av_read_frame(format, packet);
                if (readResult == AVERROR_EOF) {
                    naturalEnd = true;
                    break;
                }
                if (readResult < 0) throw std::runtime_error("Could not read audio data");
                if (packet->stream_index != audioStream) {
                    av_packet_unref(packet);
                    continue;
                }
                if (avcodec_send_packet(decoder, packet) < 0) throw std::runtime_error("Could not decode audio");
                av_packet_unref(packet);

                while (true) {
                    const int receiveResult = avcodec_receive_frame(decoder, frame);
                    if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) break;
                    if (receiveResult < 0) throw std::runtime_error("Could not decode audio frame");

                    int64_t frameMs = positionMs.load();
                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        frameMs = av_rescale_q(frame->best_effort_timestamp,
                                               format->streams[audioStream]->time_base, AVRational{1, 1000});
                    }
                    const int64_t frameDurationMs = sampleRate > 0
                                                        ? static_cast<int64_t>(frame->nb_samples) * 1000 / sampleRate
                                                        : 0;
                    if (discardUntilMs >= 0) {
                        if (frameMs + frameDurationMs <= discardUntilMs) {
                            av_frame_unref(frame);
                            continue;
                        }
                        discardUntilMs = -1;
                    }

                    std::vector<uint8_t> pcmBuffer(static_cast<size_t>(frame->nb_samples) * 2 * sizeof(int16_t));
                    uint8_t* output[2] = {pcmBuffer.data(), nullptr};
                    const uint8_t* const* input = frame->extended_data;
                    const int samples = swr_convert(resampler, output, frame->nb_samples, input, frame->nb_samples);
                    if (samples > 0) {
                        const size_t bytes = static_cast<size_t>(samples) * 2 * sizeof(int16_t);
                        auto* pcm = reinterpret_cast<int16_t*>(pcmBuffer.data());
                        const float gain = volume.load();
                        if (gain < 0.999f || gain > 1.001f) {
                            for (size_t i = 0; i < bytes / sizeof(int16_t); ++i) {
                                const int32_t scaled = static_cast<int32_t>(pcm[i] * gain);
                                pcm[i] = static_cast<int16_t>(std::clamp(scaled, -32768, 32767));
                            }
                        }
                        if (pa_simple_write(pulse, pcm, bytes, &paError) < 0) {
                            throw std::runtime_error(std::string("Audio output failed: ") + pa_strerror(paError));
                        }
                    }

                    if (frame->best_effort_timestamp != AV_NOPTS_VALUE) {
                        if (frameMs >= positionMs.load()) positionMs.store(frameMs);
                    } else {
                        positionMs.fetch_add(static_cast<int64_t>(frame->nb_samples) * 1000 / sampleRate);
                    }
                    av_frame_unref(frame);
                }
            }
            if (pulse != nullptr) pa_simple_drain(pulse, &paError);
            av_packet_free(&packet);
            av_frame_free(&frame);
        } catch (const std::exception& error) {
            setError(error.what());
            naturalEnd = false;
        } catch (...) {
            setError("Unknown playback error");
            naturalEnd = false;
        }

        cleanup();
        running.store(false);
        cv.notify_all();
        std::string finalError;
        Callback finalEnd;
        ErrorCallback finalErrorCallback;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            finalError = error;
            finalEnd = endCallback;
            finalErrorCallback = errorCallback;
        }
        if (!finalError.empty()) {
            if (finalErrorCallback) finalErrorCallback(finalError);
        } else if (naturalEnd) {
            if (finalEnd) finalEnd();
        }
    }
};

AudioEngine::AudioEngine() : impl_(std::make_unique<Impl>()) {}

AudioEngine::~AudioEngine() {
    stop();
}

void AudioEngine::play(const Track& track, const StreamSource& stream) {
    stop();
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        impl_->current = track;
        impl_->stream = stream;
        impl_->error.clear();
        impl_->positionMs.store(0);
        impl_->durationMs.store(track.durationMs);
        impl_->pendingSeek.store(-1);
        impl_->paused.store(false);
        impl_->running.store(true);
    }
    impl_->worker = std::thread([this] { impl_->run(); });
}

void AudioEngine::stop() {
    impl_->running.store(false);
    impl_->cv.notify_all();
    if (impl_->worker.joinable() && impl_->worker.get_id() != std::this_thread::get_id()) {
        impl_->worker.join();
    }
}

void AudioEngine::setPaused(bool paused) {
    impl_->paused.store(paused);
}

void AudioEngine::togglePause() {
    setPaused(!impl_->paused.load());
}

void AudioEngine::seekMs(int64_t ms) {
    impl_->pendingSeek.store(std::max<int64_t>(0, ms));
}

void AudioEngine::setVolume(float volume) {
    impl_->volume.store(std::clamp(volume, 0.0f, 2.0f));
}

void AudioEngine::reportError(const std::string& message) {
    impl_->setError(message);
    impl_->running.store(false);
    impl_->cv.notify_all();
}

PlayerSnapshot AudioEngine::snapshot() const {
    PlayerSnapshot snapshot;
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    snapshot.current = impl_->current;
    snapshot.hasCurrent = !impl_->current.id.empty();
    snapshot.playing = impl_->running.load() && !impl_->paused.load();
    snapshot.buffering = impl_->running.load() && impl_->positionMs.load() == 0;
    snapshot.positionMs = impl_->positionMs.load();
    snapshot.durationMs = impl_->durationMs.load();
    snapshot.volume = impl_->volume.load();
    snapshot.error = impl_->error;
    return snapshot;
}

void AudioEngine::setEndCallback(Callback callback) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->endCallback = std::move(callback);
}

void AudioEngine::setErrorCallback(ErrorCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->errorCallback = std::move(callback);
}

void AudioEngine::wait() {
    std::unique_lock<std::mutex> lock(impl_->stateMutex);
    impl_->cv.wait(lock, [this] { return !impl_->running.load(); });
}

}  // namespace resona

//
// Created by Laky64 on 07/10/24.
//

#include <algorithm>
#include <cstring>
#include <ntgcalls/media/audio_receiver.hpp>
#include <ntgcalls/exceptions.hpp>
#include <rtc_base/logging.h>

namespace ntgcalls {
    AudioReceiver::AudioReceiver() = default;

    AudioReceiver::~AudioReceiver() {
        std::lock_guard lock(mutex);
        // Detach the per-SSRC removal lambda we registered in open() so
        // a still-alive sink (held elsewhere via a shared_ptr) cannot
        // fire the [this]-capturing callback into a destroyed receiver.
        // Today AudioReceiver outlives the sink (sink is the local
        // shared_ptr cleared on the next line), but resetting the
        // notifier first makes the invariant explicit and survives
        // future lifetime changes.
        if (sink) {
            sink->onSsrcRemoved({});
        }
        sink = nullptr;
        resamplers.clear();
        framesCallback = nullptr;
    }

    webrtc::Resampler* AudioReceiver::resamplerFor(const uint32_t ssrc) {
        auto& slot = resamplers[ssrc];
        if (!slot) {
            slot = std::make_unique<webrtc::Resampler>();
        }
        return slot.get();
    }

    bytes::unique_binary AudioReceiver::resampleFrame(bytes::unique_binary data, const size_t size, const uint8_t channels, const uint16_t sampleRate, const uint32_t ssrc) {
        bytes::unique_binary convertedData;
        size_t preSampleSize;
        if (channels != description->channelCount) {
            switch (channels){
            case 1:
                convertedData = monoToStereo(data, size, &preSampleSize);
                break;
            case 2:
                convertedData = stereoToMono(data, size, &preSampleSize);
                break;
            default:
                RTC_LOG(LS_ERROR) << "Unsupported audio channels count: " << std::to_string(channels);
                throw InvalidParams("Unsupported audio channels count: " + std::to_string(channels));
            }
        } else {
            preSampleSize = size;
            convertedData = std::move(data);
        }
        const size_t newSize = frameSize();
        auto newFrame = bytes::make_unique_binary(newSize);
        std::memset(newFrame.get(), 0, newSize);
        if (description->sampleRate == sampleRate) {
            const size_t copyBytes = std::min(preSampleSize, newSize);
            memcpy(newFrame.get(), convertedData.get(), copyBytes);
        } else {
            const auto resampler = resamplerFor(ssrc);
            resampler->ResetIfNeeded(sampleRate, static_cast<int>(description->sampleRate), description->channelCount);
            size_t newFrameSize = 0;
            const auto resampled = resampler->Push(
                reinterpret_cast<const int16_t*>(convertedData.get()),
                preSampleSize / sizeof(int16_t),
                reinterpret_cast<int16_t*>(newFrame.get()),
                newSize / sizeof(int16_t),
                newFrameSize
            );
            if (resampled != 0) {
                RTC_LOG(LS_ERROR) << "Failed to resample audio frame";
                throw InvalidParams("Failed to resample audio frame");
            }
        }
        return std::move(newFrame);
    }

    bytes::unique_binary AudioReceiver::stereoToMono(const bytes::unique_binary& data, const size_t size, size_t *newSize) {
        *newSize = size / 2;
        auto monoData = bytes::make_unique_binary(*newSize);
        for (size_t i = 0; i < size / sizeof(int16_t); i += 2) {
            const auto left = reinterpret_cast<int16_t*>(data.get())[i];
            const auto right = reinterpret_cast<int16_t*>(data.get())[i + 1];
            const auto sum = static_cast<int32_t>(left) + static_cast<int32_t>(right);
            reinterpret_cast<int16_t*>(monoData.get())[i / 2] = static_cast<int16_t>(sum / 2);
        }
        return std::move(monoData);
    }

    bytes::unique_binary AudioReceiver::monoToStereo(const bytes::unique_binary& data, const size_t size, size_t *newSize) {
        *newSize = size * 2;
        auto stereoData = bytes::make_unique_binary(*newSize);
        for (size_t i = 0; i < size / sizeof(int16_t); i++) {
            const auto sample = reinterpret_cast<int16_t*>(data.get())[i];
            reinterpret_cast<int16_t*>(stereoData.get())[i * 2] = sample;
            reinterpret_cast<int16_t*>(stereoData.get())[i * 2 + 1] = sample;
        }
        return std::move(stereoData);
    }

    void AudioReceiver::onFrames(const std::function<void(const std::map<uint32_t, std::pair<bytes::unique_binary, size_t>>&)>& callback) {
        framesCallback = callback;
    }

    void AudioReceiver::removeSsrc(const uint32_t ssrc) {
        std::lock_guard lock(mutex);
        resamplers.erase(ssrc);
    }

    void AudioReceiver::open() {
        sink = std::make_shared<wrtc::RemoteAudioSink>([this](const std::vector<std::unique_ptr<wrtc::AudioFrame>>& samples) {
            if (!description) {
                return;
            }
            if (!weakSink.lock()) {
                return;
            }
            std::lock_guard lock(mutex);
            std::map<uint32_t, std::pair<bytes::unique_binary, size_t>> processedFrames;
            for (const auto& frame: samples) {
                try {
                    bytes::unique_binary data = bytes::make_unique_binary(frame->size);
                    memcpy(data.get(), frame->data.get(), frame->size);
                    processedFrames.emplace(
                        frame->ssrc,
                        std::pair{
                            resampleFrame(
                                std::move(data),
                                frame->size,
                                frame->channels,
                                frame->sampleRate,
                                frame->ssrc
                            ),
                            frameSize()
                        }
                    );
                } catch (const InvalidParams& e) {
                    RTC_LOG(LS_ERROR) << "Failed to adapt audio frame: " << e.what();
                }
            }
            frames++;
            (void) framesCallback(processedFrames);
        });
        weakSink = sink;
        // Subscribe to per-SSRC teardown notifications from the sink so
        // we can release the matching resampler the moment a channel
        // goes away — keeps the resamplers map bounded over long calls
        // and prevents an SSRC reuse from inheriting stale filter state.
        sink->onSsrcRemoved([this](const uint32_t ssrc) {
            removeSsrc(ssrc);
        });
    }

    std::weak_ptr<wrtc::RemoteAudioSink> AudioReceiver::remoteSink() {
        return sink;
    }
} // ntgcalls
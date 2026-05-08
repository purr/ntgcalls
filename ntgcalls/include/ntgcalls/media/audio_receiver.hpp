//
// Created by Laky64 on 07/10/24.
//

#pragma once
#include <map>
#include <unordered_map>
#include <wrtc/interfaces/media/remote_audio_sink.hpp>
#include <ntgcalls/media/base_receiver.hpp>
#include <ntgcalls/media/audio_sink.hpp>
#include <common_audio/resampler/include/resampler.h>
#include <wrtc/utils/binary.hpp>
#include <wrtc/utils/synchronized_callback.hpp>

namespace ntgcalls {

    class AudioReceiver final: public AudioSink, public BaseReceiver {
        wrtc::synchronized_callback<const std::map<uint32_t, std::pair<bytes::unique_binary, size_t>>&> framesCallback;
        std::shared_ptr<wrtc::RemoteAudioSink> sink;
        // Per-SSRC: webrtc::Resampler carries filter state across Push() calls; sharing one across SSRCs corrupts pitch.
        std::unordered_map<uint32_t, std::unique_ptr<webrtc::Resampler>> resamplers;

        webrtc::Resampler* resamplerFor(uint32_t ssrc);

        bytes::unique_binary resampleFrame(bytes::unique_binary data, size_t size, uint8_t channels, uint16_t sampleRate, uint32_t ssrc);

        static bytes::unique_binary stereoToMono(const bytes::unique_binary& data, size_t size, size_t *newSize);

        static bytes::unique_binary monoToStereo(const bytes::unique_binary& data, size_t size, size_t *newSize);

    public:
        AudioReceiver();

        ~AudioReceiver() override;

        std::weak_ptr<wrtc::RemoteAudioSink> remoteSink();

        void onFrames(const std::function<void(const std::map<uint32_t, std::pair<bytes::unique_binary, size_t>>&)>& callback);

        void open() override;
    };

} // ntgcalls

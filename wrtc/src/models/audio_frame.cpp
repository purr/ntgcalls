//
// Created by Laky64 on 07/10/24.
//

#include <cstring>
#include <wrtc/models/audio_frame.hpp>

namespace wrtc {
    AudioFrame::AudioFrame(const uint32_t ssrc): ssrc(ssrc) {}

    void AudioFrame::setData(const int16_t* src, const size_t bytes) {
        size = bytes;
        if (bytes == 0 || src == nullptr) {
            data.reset();
            return;
        }
        const size_t elements = (bytes + sizeof(int16_t) - 1) / sizeof(int16_t);
        data.reset(new int16_t[elements]);
        std::memcpy(data.get(), src, bytes);
    }
} // wrtc

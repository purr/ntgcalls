//
// Created by Laky64 on 07/10/24.
//

#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>

namespace wrtc {

    class AudioFrame {
    public:
        uint32_t ssrc;
        // Owned PCM buffer. The previous design held a borrowed pointer to
        // libwebrtc's per-channel render buffer, which is reused on the next
        // NetEq tick. Once the AudioFrame was buffered for cross-tick mixing,
        // reads became use-after-free, manifesting as garbled / half-pitch
        // audio for whichever sources were not the last to deliver in a tick.
        std::unique_ptr<int16_t[]> data;
        size_t size = 0;
        int sampleRate = 0;
        size_t channels = 0;

        explicit AudioFrame(uint32_t ssrc);

        void setData(const int16_t* src, size_t bytes);
    };

} // wrtc

//
// Created by Laky64 on 07/10/24.
//

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <functional>
#include <wrtc/models/audio_frame.hpp>
#include <wrtc/interfaces/media/remote_media_interface.hpp>

namespace wrtc {

    class RemoteAudioSink final: public RemoteMediaInterface, public std::enable_shared_from_this<RemoteAudioSink> {
        std::atomic<uint32_t> numSources;
        std::mutex mutex;
        // Latest frame per SSRC. The previous design used an append-only vector
        // gated on (size >= numSources); when numSources drifted high (e.g. an
        // audio channel was evicted past the 10-channel cap without paired
        // removeSource()) the gate accumulated multiple ticks of frames and
        // mixed them at one go, halving the effective playback rate for the
        // affected sources (audible as half-pitch / "deep" voice).
        std::unordered_map<uint32_t, std::unique_ptr<AudioFrame>> latestBySsrc;
        std::chrono::steady_clock::time_point lastIngest;
        std::function<void(const std::vector<std::unique_ptr<AudioFrame>>&)> framesCallback;
        // Optional notifier the consumer (ntgcalls' AudioReceiver) can
        // register so it gets a chance to drop its own per-SSRC state
        // (resampler entry, etc.) the moment a channel goes away.  Stored
        // separately from framesCallback because it fires at channel
        // teardown, not at every frame ingest.
        std::function<void(uint32_t)> ssrcRemovedCallback;
        // Drain bookkeeping for the per-SSRC notifier.  ``removeSsrc``
        // captures the registered callback under ``mutex`` then fires it
        // OUTSIDE the lock (so consumers may take their own mutex
        // without deadlocking against ours).  That window is when a
        // racing ``onSsrcRemoved({})`` clear from a destructor could
        // otherwise leave a stack-local copy of the previous callback
        // dangling — its ``[this]`` capture is no longer valid by the
        // time it fires.  ``ssrcInflight`` counts in-flight notifier
        // invocations; ``onSsrcRemoved`` waits on ``drainCv`` until
        // the count drops to zero before returning, guaranteeing no
        // surviving notifier copy can outlive the caller's state.
        std::atomic<int> ssrcInflight{0};
        std::mutex drainMutex;
        std::condition_variable drainCv;

    public:
        explicit RemoteAudioSink(const std::function<void(const std::vector<std::unique_ptr<AudioFrame>>&)>& callback);

        ~RemoteAudioSink() override;

        void sendData(std::unique_ptr<AudioFrame> frame);

        void addSource();

        void removeSource();

        void updateAudioSourceCount(int count);

        // Drop any cached state for a specific SSRC and notify the
        // registered consumer.  Call from the layer that owns the
        // incoming audio channel when that channel is torn down.
        void removeSsrc(uint32_t ssrc);

        // Register the per-SSRC teardown notifier.  Replaces any
        // previously-registered handler.  Pass {} to clear.
        void onSsrcRemoved(std::function<void(uint32_t)> callback);
    };

} // wrtc

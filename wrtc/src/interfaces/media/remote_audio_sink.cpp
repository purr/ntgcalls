//
// Created by Laky64 on 07/10/24.
//

#include <wrtc/interfaces/media/remote_audio_sink.hpp>

namespace wrtc {
    RemoteAudioSink::RemoteAudioSink(const std::function<void(const std::vector<std::unique_ptr<AudioFrame>>&)>& callback)
        : numSources(0), lastIngest(std::chrono::steady_clock::now()) {
        framesCallback = callback;
    }

    RemoteAudioSink::~RemoteAudioSink() {
        framesCallback = nullptr;
        std::lock_guard lock(mutex);
        latestBySsrc.clear();
    }

    void RemoteAudioSink::sendData(std::unique_ptr<AudioFrame> frame) {
        // Render ticks fire each source's OnData back-to-back (microseconds apart) at 100Hz. Close a batch on either (a) all advertised sources delivered, or (b) >=8ms gap signaling next tick. Decouples playback rate from numSources accuracy.
        std::vector<std::unique_ptr<AudioFrame>> snapshot;
        std::function<void(const std::vector<std::unique_ptr<AudioFrame>>&)> cb;
        {
            std::lock_guard lock(mutex);
            const auto now = std::chrono::steady_clock::now();
            const auto sinceLastIngest = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastIngest).count();

            if (!latestBySsrc.empty() && sinceLastIngest >= 8) {
                snapshot.reserve(latestBySsrc.size());
                for (auto& [_, f] : latestBySsrc) {
                    snapshot.push_back(std::move(f));
                }
                latestBySsrc.clear();
            }

            const auto ssrc = frame->ssrc;
            latestBySsrc[ssrc] = std::move(frame);
            lastIngest = now;

            if (snapshot.empty()) {
                const auto target = numSources.load();
                if (target > 0 && latestBySsrc.size() >= target) {
                    snapshot.reserve(latestBySsrc.size());
                    for (auto& [_, f] : latestBySsrc) {
                        snapshot.push_back(std::move(f));
                    }
                    latestBySsrc.clear();
                }
            }
            cb = framesCallback;
        }
        if (cb && !snapshot.empty()) {
            cb(snapshot);
        }
    }

    void RemoteAudioSink::addSource() {
        ++numSources;
    }

    void RemoteAudioSink::removeSource() {
        // CAS to avoid underflow if removeSource() races (it currently runs on the network thread only, but the guard makes the invariant explicit).
        uint32_t cur = numSources.load();
        while (cur > 0 && !numSources.compare_exchange_weak(cur, cur - 1)) {}
    }

    void RemoteAudioSink::updateAudioSourceCount(const int count) {
        numSources = count < 0 ? 0 : static_cast<uint32_t>(count);
    }

    void RemoteAudioSink::removeSsrc(const uint32_t ssrc) {
        // Drop pending frame for this ssrc and capture the notifier under
        // the lock; invoke the notifier OUTSIDE the lock so a consumer
        // that needs to take its own mutex can't deadlock against us.
        // ``ssrcInflight`` is incremented under ``mutex`` (paired with
        // notify capture) so a concurrent ``onSsrcRemoved`` clear can
        // observe and wait for us — see ``onSsrcRemoved`` for the drain
        // pattern.  Without this, a destructor that clears the callback
        // and then frees state captured by the previous callback could
        // race against an in-flight notifier copy → use-after-free.
        std::function<void(uint32_t)> notify;
        {
            std::lock_guard lock(mutex);
            latestBySsrc.erase(ssrc);
            notify = ssrcRemovedCallback;
            if (notify) {
                ssrcInflight.fetch_add(1, std::memory_order_acq_rel);
            }
        }
        if (notify) {
            try {
                notify(ssrc);
            } catch (...) {
                // Swallow consumer exceptions; we MUST still decrement
                // the in-flight counter so any waiter on ``drainCv`` is
                // released, otherwise ``onSsrcRemoved`` deadlocks.
            }
            if (ssrcInflight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard cv_lk(drainMutex);
                drainCv.notify_all();
            }
        }
    }

    void RemoteAudioSink::onSsrcRemoved(std::function<void(uint32_t)> callback) {
        // Replace the callback atomically.  Then wait for any in-flight
        // notifier (which captured the PREVIOUS callback by value) to
        // finish before returning, so the caller can safely destroy any
        // state the previous callback's ``[this]`` capture pointed at.
        // No-op fast path when no callbacks are in flight.
        {
            std::lock_guard lock(mutex);
            ssrcRemovedCallback = std::move(callback);
        }
        std::unique_lock cv_lk(drainMutex);
        drainCv.wait(cv_lk, [this] {
            return ssrcInflight.load(std::memory_order_acquire) == 0;
        });
    }
} // wrtc

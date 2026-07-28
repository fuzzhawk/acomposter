// Non-interleaved float audio buffer.
//
// The engine works exclusively in de-interleaved 32-bit float. Interleaving only
// happens at the two edges: the WASAPI device and VST2 plugins that ask for it.
#pragma once

#include "Types.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace acm {

class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(int channels, int frames) { resize(channels, frames); }

    // Reallocates. Never call this from the audio thread - the graph sizes every
    // buffer up front in prepare().
    void resize(int channels, int frames) {
        channels_ = std::max(0, channels);
        capacity_ = std::max(0, frames);
        frames_ = capacity_;
        storage_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_), 0.0f);
        pointers_.resize(static_cast<std::size_t>(channels_));
        for (int c = 0; c < channels_; ++c)
            pointers_[static_cast<std::size_t>(c)] = storage_.data() + static_cast<std::size_t>(c) * capacity_;
    }

    // Cheap, allocation-free re-framing within the already-reserved capacity.
    void setActiveFrames(int frames) noexcept {
        frames_ = clampValue(frames, 0, capacity_);
    }

    int channels() const noexcept { return channels_; }
    int frames() const noexcept { return frames_; }
    int capacity() const noexcept { return capacity_; }

    float* channel(int c) noexcept { return pointers_[static_cast<std::size_t>(c)]; }
    const float* channel(int c) const noexcept { return pointers_[static_cast<std::size_t>(c)]; }

    float* const* data() noexcept { return pointers_.data(); }
    const float* const* data() const noexcept { return pointers_.data(); }

    void clear() noexcept {
        for (int c = 0; c < channels_; ++c)
            std::memset(pointers_[static_cast<std::size_t>(c)], 0, sizeof(float) * static_cast<std::size_t>(frames_));
    }

    void clearChannel(int c) noexcept {
        std::memset(pointers_[static_cast<std::size_t>(c)], 0, sizeof(float) * static_cast<std::size_t>(frames_));
    }

    void copyFrom(const AudioBuffer& src) noexcept {
        const int ch = std::min(channels_, src.channels());
        const int n = std::min(frames_, src.frames());
        for (int c = 0; c < ch; ++c)
            std::memcpy(pointers_[static_cast<std::size_t>(c)], src.channel(c), sizeof(float) * static_cast<std::size_t>(n));
        // Any channels the source does not provide are silence, not stale audio.
        for (int c = ch; c < channels_; ++c)
            clearChannel(c);
    }

    void addFrom(const AudioBuffer& src, float gain = 1.0f) noexcept {
        const int ch = std::min(channels_, src.channels());
        const int n = std::min(frames_, src.frames());
        for (int c = 0; c < ch; ++c) {
            float* dst = pointers_[static_cast<std::size_t>(c)];
            const float* s = src.channel(c);
            for (int i = 0; i < n; ++i) dst[i] += s[i] * gain;
        }
    }

    void applyGain(float gain) noexcept {
        for (int c = 0; c < channels_; ++c) {
            float* d = pointers_[static_cast<std::size_t>(c)];
            for (int i = 0; i < frames_; ++i) d[i] *= gain;
        }
    }

    void applyGainRamp(float from, float to) noexcept {
        if (frames_ <= 0) return;
        const float step = (to - from) / static_cast<float>(frames_);
        for (int c = 0; c < channels_; ++c) {
            float* d = pointers_[static_cast<std::size_t>(c)];
            float g = from;
            for (int i = 0; i < frames_; ++i) { d[i] *= g; g += step; }
        }
    }

    float peak(int c) const noexcept {
        const float* d = pointers_[static_cast<std::size_t>(c)];
        float m = 0.0f;
        for (int i = 0; i < frames_; ++i) {
            const float a = d[i] < 0.0f ? -d[i] : d[i];
            if (a > m) m = a;
        }
        return m;
    }

private:
    std::vector<float> storage_;
    std::vector<float*> pointers_;
    int channels_ = 0;
    int frames_ = 0;
    int capacity_ = 0;
};

// ---------------------------------------------------------------------------
// Bus views handed to Node::process()
//
// A bus points into buffers the graph owns. `connected` distinguishes "silence
// because nothing is patched in" from "silence because the source is quiet",
// which several nodes (looper arming, sample player gating) care about.
// ---------------------------------------------------------------------------

struct AudioBus {
    float* const* channels = nullptr;
    int numChannels = 0;
    int numFrames = 0;
    bool connected = false;

    float* chan(int c) noexcept { return channels[c]; }
    const float* chan(int c) const noexcept { return channels[c]; }

    void clear() noexcept {
        for (int c = 0; c < numChannels; ++c)
            std::memset(channels[c], 0, sizeof(float) * static_cast<std::size_t>(numFrames));
    }

    // Reads a channel with wrap-around so a mono source feeding a stereo bus (or
    // vice versa) behaves sensibly instead of reading past the end.
    const float* chanWrapped(int c) const noexcept {
        return channels[numChannels > 0 ? (c % numChannels) : 0];
    }
};

} // namespace acm

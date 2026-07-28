// An immutable block of decoded audio plus the metadata the sample player needs.
//
// Once loaded, a SampleBuffer is never modified. That is what lets the sample
// player hand it to the audio thread through an AtomicResource: swapping a file
// is a pointer store, and the old buffer is freed on the message thread once the
// audio thread has moved on.
#pragma once

#include "../core/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace acm {

class SampleBuffer {
public:
    SampleBuffer() = default;
    SampleBuffer(int channels, std::int64_t frames, double sampleRate);

    // -- audio -------------------------------------------------------------
    int channels() const noexcept { return channels_; }
    std::int64_t frames() const noexcept { return frames_; }
    double sampleRate() const noexcept { return sampleRate_; }
    double durationSeconds() const noexcept {
        return sampleRate_ > 0.0 ? static_cast<double>(frames_) / sampleRate_ : 0.0;
    }
    bool empty() const noexcept { return frames_ <= 0 || channels_ <= 0; }

    // Planar storage: channel `c` is contiguous.
    const float* channel(int c) const noexcept {
        return data_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(frames_);
    }
    float* channelForWrite(int c) noexcept {
        return data_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(frames_);
    }

    // Reads channel `c` with the channel-count wrap the graph uses elsewhere, so
    // a mono file feeding a stereo player lands in both sides.
    const float* channelWrapped(int c) const noexcept {
        return channel(channels_ > 0 ? (c % channels_) : 0);
    }

    // -- metadata ----------------------------------------------------------
    std::string sourcePath;
    std::string displayName;

    // Loop points read from the file's 'smpl' chunk, when it has one. Sample
    // players adopt these as their initial loop region.
    bool hasEmbeddedLoop = false;
    std::int64_t loopStart = 0;
    std::int64_t loopEnd = 0;

    // Tempo inferred from the file name ("break_174bpm.wav") or from an ACID
    // chunk. Zero when unknown.
    double detectedBpm = 0.0;

    // -- waveform overview -------------------------------------------------
    // Min/max envelope, precomputed once at load so the patcher can draw a
    // waveform without touching the sample data every frame.
    struct Overview {
        int buckets = 0;
        std::vector<float> minimum;  // buckets entries, mono-summed
        std::vector<float> maximum;
        std::vector<float> rms;
    };

    const Overview& overview() const noexcept { return overview_; }
    void buildOverview(int buckets = 2048);

    // Peak of the whole file, for normalise-on-load.
    float peakLevel() const noexcept { return peak_; }
    void computePeak();

    std::size_t memoryBytes() const noexcept { return data_.size() * sizeof(float); }

private:
    std::vector<float> data_;
    int channels_ = 0;
    std::int64_t frames_ = 0;
    double sampleRate_ = 48000.0;
    float peak_ = 0.0f;
    Overview overview_;
};

using SampleBufferPtr = std::shared_ptr<const SampleBuffer>;

} // namespace acm

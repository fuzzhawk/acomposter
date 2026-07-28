#include "SampleBuffer.h"

#include <algorithm>
#include <cmath>

namespace acm {

SampleBuffer::SampleBuffer(int channels, std::int64_t frames, double sampleRate)
    : channels_(clampValue(channels, 0, kMaxChannelsPerPort)),
      frames_(frames > 0 ? frames : 0),
      sampleRate_(sampleRate > 0.0 ? sampleRate : 48000.0) {
    data_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(frames_), 0.0f);
}

void SampleBuffer::computePeak() {
    float peak = 0.0f;
    for (float v : data_) {
        const float a = v < 0.0f ? -v : v;
        if (a > peak) peak = a;
    }
    peak_ = peak;
}

void SampleBuffer::buildOverview(int buckets) {
    overview_ = Overview{};
    if (empty()) return;

    // No point in more buckets than frames; a short one-shot gets an exact
    // envelope rather than a stretched one.
    buckets = clampValue(buckets, 16, 8192);
    if (static_cast<std::int64_t>(buckets) > frames_)
        buckets = static_cast<int>(frames_);

    overview_.buckets = buckets;
    overview_.minimum.resize(static_cast<std::size_t>(buckets));
    overview_.maximum.resize(static_cast<std::size_t>(buckets));
    overview_.rms.resize(static_cast<std::size_t>(buckets));

    const double framesPerBucket = static_cast<double>(frames_) / static_cast<double>(buckets);
    const float channelScale = 1.0f / static_cast<float>(channels_);

    for (int b = 0; b < buckets; ++b) {
        const auto start = static_cast<std::int64_t>(static_cast<double>(b) * framesPerBucket);
        auto end = static_cast<std::int64_t>(static_cast<double>(b + 1) * framesPerBucket);
        if (end <= start) end = start + 1;
        if (end > frames_) end = frames_;

        float lo = 0.0f, hi = 0.0f;
        double sumOfSquares = 0.0;

        for (std::int64_t i = start; i < end; ++i) {
            // Mono sum so the overview reads as one waveform regardless of the
            // channel count.
            float mono = 0.0f;
            for (int c = 0; c < channels_; ++c) mono += channel(c)[i];
            mono *= channelScale;

            if (mono < lo) lo = mono;
            if (mono > hi) hi = mono;
            sumOfSquares += static_cast<double>(mono) * mono;
        }

        const auto count = static_cast<double>(end - start);
        overview_.minimum[static_cast<std::size_t>(b)] = lo;
        overview_.maximum[static_cast<std::size_t>(b)] = hi;
        overview_.rms[static_cast<std::size_t>(b)] =
            count > 0.0 ? static_cast<float>(std::sqrt(sumOfSquares / count)) : 0.0f;
    }
}

} // namespace acm

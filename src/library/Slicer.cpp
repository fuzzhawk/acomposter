#include "Slicer.h"

#include "../dsp/Fft.h"

#include <algorithm>
#include <cmath>

namespace acm::library {
namespace {

constexpr int kFftSize = 1024;
constexpr int kHop = 256;

} // namespace

std::vector<std::int64_t> findSlicePoints(const SampleBuffer& buffer,
                                          const SliceSettings& settings) {
    std::vector<std::int64_t> points;
    if (buffer.empty() || buffer.sampleRate() <= 0.0) return points;

    const std::int64_t frames = buffer.frames();
    if (frames < kFftSize * 2) return points;

    // Mono, because a transient is a transient on both sides and comparing the
    // two would only ever produce the same answer twice.
    std::vector<float> mono(static_cast<std::size_t>(frames), 0.0f);
    for (int c = 0; c < buffer.channels(); ++c) {
        const float* data = buffer.channel(c);
        for (std::int64_t i = 0; i < frames; ++i)
            mono[static_cast<std::size_t>(i)] += data[i];
    }
    const float scale = 1.0f / static_cast<float>(std::max(1, buffer.channels()));
    for (float& sample : mono) sample *= scale;

    // -- spectral flux ------------------------------------------------------
    // The sum of positive changes per bin, window to window. Positive only:
    // energy leaving is a note ending, and a slice does not start there.
    dsp::Fft fft(kFftSize);
    std::vector<float> magnitude;
    std::vector<float> previous;
    std::vector<float> flux;

    for (std::int64_t offset = 0; offset + kFftSize <= frames; offset += kHop) {
        fft.magnitude(mono.data() + offset, kFftSize, magnitude);

        if (previous.size() == magnitude.size()) {
            float sum = 0.0f;
            for (std::size_t bin = 0; bin < magnitude.size(); ++bin) {
                const float rise = magnitude[bin] - previous[bin];
                if (rise > 0.0f) sum += rise;
            }
            flux.push_back(sum);
        } else {
            flux.push_back(0.0f);
        }

        previous = magnitude;
    }

    if (flux.size() < 4) return points;

    // -- peak picking -------------------------------------------------------
    // Against a running mean rather than a fixed threshold, so a loop that gets
    // louder does not stop producing slices half way through.
    constexpr int kWindow = 12;
    const auto minimumGapFrames =
        static_cast<std::int64_t>(settings.minimumGapSeconds * buffer.sampleRate());

    std::int64_t lastPoint = -minimumGapFrames * 2;

    // The head of the file is always a slice: the first hit usually starts at
    // or before the first flux window, and dropping it would silently discard
    // one hit from every loop.
    std::int64_t firstSounding = 0;
    for (std::int64_t i = 0; i < frames; ++i) {
        if (std::fabs(mono[static_cast<std::size_t>(i)]) > 0.001f) { firstSounding = i; break; }
    }
    points.push_back(firstSounding);
    lastPoint = firstSounding;

    for (std::size_t i = 1; i + 1 < flux.size(); ++i) {
        const auto begin = static_cast<std::size_t>(std::max<std::int64_t>(
            0, static_cast<std::int64_t>(i) - kWindow));
        const auto end = std::min(flux.size(), i + kWindow);

        double mean = 0.0;
        for (std::size_t k = begin; k < end; ++k) mean += flux[k];
        mean /= static_cast<double>(end - begin);

        // A local maximum, and above the neighbourhood by the sensitivity
        // factor. Both conditions: the threshold alone fires on the whole
        // plateau of a long attack.
        if (flux[i] <= flux[i - 1] || flux[i] < flux[i + 1]) continue;
        if (static_cast<double>(flux[i]) < mean * settings.sensitivity) continue;

        // The window's start, not its centre: a slice has to include the
        // attack that identified it.
        const auto frame = static_cast<std::int64_t>(i) * kHop;
        if (frame - lastPoint < minimumGapFrames) continue;

        points.push_back(frame);
        lastPoint = frame;
    }

    // -- drop the runts -----------------------------------------------------
    const auto minimumLength =
        static_cast<std::int64_t>(settings.minimumLengthSeconds * buffer.sampleRate());

    std::vector<std::int64_t> kept;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::int64_t end = (i + 1 < points.size()) ? points[i + 1] : frames;
        if (end - points[i] >= minimumLength) kept.push_back(points[i]);
    }

    return kept;
}

std::shared_ptr<SampleBuffer> extractSlice(const SampleBuffer& buffer,
                                           std::int64_t start, std::int64_t end) {
    if (buffer.empty()) return nullptr;

    start = std::clamp<std::int64_t>(start, 0, buffer.frames());
    end = std::clamp<std::int64_t>(end, start, buffer.frames());

    const std::int64_t length = end - start;
    if (length <= 0) return nullptr;

    auto out = std::make_shared<SampleBuffer>(buffer.channels(), length, buffer.sampleRate());

    for (int c = 0; c < buffer.channels(); ++c) {
        const float* source = buffer.channel(c);
        float* destination = out->channelForWrite(c);
        for (std::int64_t i = 0; i < length; ++i) destination[i] = source[start + i];
    }

    // A slice cut mid-cycle clicks on both ends. A couple of milliseconds of
    // fade is inaudible on a drum hit and removes it.
    const auto fade = std::min<std::int64_t>(length / 2,
                                             static_cast<std::int64_t>(buffer.sampleRate() * 0.002));
    for (int c = 0; c < buffer.channels(); ++c) {
        float* data = out->channelForWrite(c);
        for (std::int64_t i = 0; i < fade; ++i) {
            const float gain = static_cast<float>(i) / static_cast<float>(fade);
            data[i] *= gain;
            data[length - 1 - i] *= gain;
        }
    }

    out->computePeak();
    return out;
}

} // namespace acm::library

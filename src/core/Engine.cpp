#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace acm {
namespace {

inline float dbToGain(float db) noexcept {
    return db <= -95.0f ? 0.0f : std::pow(10.0f, db * 0.05f);
}

// Soft ceiling. Linear below the knee so normal levels are untouched, then a
// tanh curve that asymptotes just under full scale.
inline float softClip(float x) noexcept {
    constexpr float knee = 0.7f;
    if (x > -knee && x < knee) return x;
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float over = (x * sign) - knee;
    return sign * (knee + (1.0f - knee) * std::tanh(over / (1.0f - knee)));
}

} // namespace

Engine::Engine() {
    graph_.setClock(&blockCounter_);
}

Engine::~Engine() = default;

void Engine::prepare(double sampleRate, int maxBlockSize, int inputChannels, int outputChannels) {
    prepared_.store(false, std::memory_order_release);

    sampleRate_ = clampValue(sampleRate, kMinSampleRate, kMaxSampleRate);
    maxBlockSize_ = clampValue(maxBlockSize, 16, kMaxBlockSize);

    deviceInput_.resize(clampValue(inputChannels, 0, kMaxChannelsPerPort), maxBlockSize_);
    deviceOutput_.resize(clampValue(outputChannels, 1, kMaxChannelsPerPort), maxBlockSize_);

    transport_.prepare(sampleRate_);
    graph_.prepare(sampleRate_, maxBlockSize_);

    masterSmoothing_.reset(sampleRate_, 0.03);
    masterSmoothing_.setCurrentAndTarget(masterGainLinear_.load(std::memory_order_relaxed));

    resetStats();
    prepared_.store(true, std::memory_order_release);
}

void Engine::release() {
    prepared_.store(false, std::memory_order_release);
}

void Engine::setMasterGainDb(float db) noexcept {
    db = clampValue(db, -96.0f, 12.0f);
    masterGainDb_.store(db, std::memory_order_relaxed);
    masterGainLinear_.store(dbToGain(db), std::memory_order_relaxed);
}

void Engine::processInterleaved(const float* input, int inputChannels,
                                float* output, int outputChannels, int frames) {
    if (output == nullptr || frames <= 0 || outputChannels <= 0) return;

    const std::size_t outSamples = static_cast<std::size_t>(frames) * static_cast<std::size_t>(outputChannels);

    if (!prepared_.load(std::memory_order_acquire)) {
        std::memset(output, 0, outSamples * sizeof(float));
        return;
    }

    // A device that hands us more than we prepared for is a configuration
    // change we cannot service from here; emit silence and count the drop-out.
    if (frames > maxBlockSize_) {
        xruns_.fetch_add(1, std::memory_order_relaxed);
        std::memset(output, 0, outSamples * sizeof(float));
        return;
    }

    const ScopedNoDenormals noDenormals;
    const auto blockStart = std::chrono::steady_clock::now();

    if (panicRequested_.exchange(false, std::memory_order_acquire)) {
        graph_.reset();
        deviceInput_.clear();
        deviceOutput_.clear();
    }

    deviceInput_.setActiveFrames(frames);
    deviceOutput_.setActiveFrames(frames);

    // De-interleave capture.
    const int inCh = std::min(inputChannels, deviceInput_.channels());
    if (input != nullptr && inCh > 0) {
        for (int c = 0; c < inCh; ++c) {
            float* dst = deviceInput_.channel(c);
            const float* src = input + c;
            for (int i = 0; i < frames; ++i) dst[i] = src[static_cast<std::size_t>(i) * inputChannels];
        }
        for (int c = inCh; c < deviceInput_.channels(); ++c) deviceInput_.clearChannel(c);
    } else {
        deviceInput_.clear();
    }

    deviceOutput_.clear();

    TransportState state;
    transport_.beginBlock(state, frames);
    state.sampleRate = sampleRate_;

    const std::uint64_t streamFrame = blockCounter_.load(std::memory_order_relaxed)
                                    * static_cast<std::uint64_t>(maxBlockSize_);

    graph_.render(state, frames, streamFrame, &deviceInput_, &deviceOutput_);

    // Master section.
    const float targetGain = masterMuted_.load(std::memory_order_relaxed)
                                 ? 0.0f
                                 : masterGainLinear_.load(std::memory_order_relaxed);
    masterSmoothing_.setTarget(targetGain);

    const bool limit = limiter_.load(std::memory_order_relaxed);
    const int busCh = deviceOutput_.channels();
    float peak[2] = { 0.0f, 0.0f };

    for (int i = 0; i < frames; ++i) {
        const float g = masterSmoothing_.next();
        for (int c = 0; c < busCh; ++c) {
            float v = deviceOutput_.channel(c)[i] * g;
            if (limit) v = softClip(v);
            if (!std::isfinite(v)) v = 0.0f;
            deviceOutput_.channel(c)[i] = v;

            const float a = v < 0.0f ? -v : v;
            const int meter = c & 1;
            if (a > peak[meter]) peak[meter] = a;
        }
    }

    // Interleave to the device, wrapping if it wants more channels than the bus
    // carries (a 5.1 endpoint gets the stereo bus repeated rather than silence).
    for (int c = 0; c < outputChannels; ++c) {
        const float* src = deviceOutput_.channel(busCh > 0 ? (c % busCh) : 0);
        float* dst = output + c;
        for (int i = 0; i < frames; ++i) dst[static_cast<std::size_t>(i) * outputChannels] = src[i];
    }

    // Meters fall back at roughly 20 dB/s so peaks stay readable.
    const float decay = std::exp(-static_cast<float>(frames) / static_cast<float>(sampleRate_ * 0.35));
    for (int c = 0; c < 2; ++c) {
        const float previous = masterPeak_[c].load(std::memory_order_relaxed) * decay;
        masterPeak_[c].store(peak[c] > previous ? peak[c] : previous, std::memory_order_relaxed);
    }

    // Load, as a fraction of the wall-clock budget for this block.
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - blockStart).count();
    const double budget = static_cast<double>(frames) / sampleRate_;
    if (budget > 0.0) {
        const float instant = static_cast<float>(elapsed / budget);
        const float smoothed = cpuLoad_.load(std::memory_order_relaxed) * 0.9f + instant * 0.1f;
        cpuLoad_.store(smoothed, std::memory_order_relaxed);
        if (instant > peakCpuLoad_.load(std::memory_order_relaxed))
            peakCpuLoad_.store(instant, std::memory_order_relaxed);
        if (instant >= 1.0f) xruns_.fetch_add(1, std::memory_order_relaxed);
    }

    lastBlockSize_.store(frames, std::memory_order_relaxed);
    blockCounter_.fetch_add(1, std::memory_order_release);
}

EngineStats Engine::stats() const noexcept {
    EngineStats s;
    s.cpuLoad = cpuLoad_.load(std::memory_order_relaxed);
    s.peakCpuLoad = peakCpuLoad_.load(std::memory_order_relaxed);
    s.xruns = xruns_.load(std::memory_order_relaxed);
    s.sampleRate = sampleRate_;
    s.blockSize = lastBlockSize_.load(std::memory_order_relaxed);
    s.blocksRendered = blockCounter_.load(std::memory_order_relaxed);
    s.nodeCount = static_cast<int>(graph_.nodeCount());
    s.feedbackEdges = graph_.scheduledFeedbackEdges();
    return s;
}

void Engine::resetStats() noexcept {
    cpuLoad_.store(0.0f, std::memory_order_relaxed);
    peakCpuLoad_.store(0.0f, std::memory_order_relaxed);
    xruns_.store(0, std::memory_order_relaxed);
}

void Engine::serviceFromMessageThread() {
    graph_.collectGarbage();
    for (const auto& n : graph_.nodes())
        n->serviceFromMessageThread();
}

} // namespace acm

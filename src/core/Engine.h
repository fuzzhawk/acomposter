// The audio engine: owns the graph and the transport, and is the only thing the
// audio device talks to.
//
// The device hands over interleaved frames; everything above this line works in
// de-interleaved float. The engine also carries the per-block housekeeping the
// rest of the app relies on: the block counter that makes lock-free hand-offs
// safe, CPU load measurement, master metering, and the drop-out counter.
#pragma once

#include "AudioBuffer.h"
#include "Denormals.h"
#include "Graph.h"
#include "Transport.h"
#include "Types.h"

#include <atomic>

namespace acm {

struct EngineStats {
    float cpuLoad = 0.0f;        // 0..1 of the block budget
    float peakCpuLoad = 0.0f;
    int xruns = 0;
    double sampleRate = 48000.0;
    int blockSize = 0;
    std::uint64_t blocksRendered = 0;
    int nodeCount = 0;
    int feedbackEdges = 0;
};

class Engine {
public:
    Engine();
    ~Engine();

    Graph& graph() noexcept { return graph_; }
    const Graph& graph() const noexcept { return graph_; }
    Transport& transport() noexcept { return transport_; }
    const Transport& transport() const noexcept { return transport_; }

    // -- device lifecycle (message thread, callback stopped) ---------------
    void prepare(double sampleRate, int maxBlockSize, int inputChannels, int outputChannels);
    void release();

    // -- audio thread ------------------------------------------------------
    // `input` may be null when the device has no capture stream.
    void processInterleaved(const float* input, int inputChannels,
                            float* output, int outputChannels, int frames);

    // -- master section ----------------------------------------------------
    void setMasterGainDb(float db) noexcept;
    float masterGainDb() const noexcept { return masterGainDb_.load(std::memory_order_relaxed); }

    void setMasterMuted(bool m) noexcept { masterMuted_.store(m, std::memory_order_relaxed); }
    bool masterMuted() const noexcept { return masterMuted_.load(std::memory_order_relaxed); }

    // A gentle tanh-shaped ceiling on the master bus. On by default: a patch
    // with a runaway feedback loop should not be able to destroy a PA.
    void setMasterLimiterEnabled(bool e) noexcept { limiter_.store(e, std::memory_order_relaxed); }
    bool masterLimiterEnabled() const noexcept { return limiter_.load(std::memory_order_relaxed); }

    float masterPeak(int channel) const noexcept {
        return masterPeak_[channel & 1].load(std::memory_order_relaxed);
    }

    // Silences the graph and resets every node. Bound to a panic key.
    void panic() noexcept { panicRequested_.store(true, std::memory_order_release); }

    // -- housekeeping ------------------------------------------------------
    const BlockCounter& blockCounter() const noexcept { return blockCounter_; }
    EngineStats stats() const noexcept;
    void resetStats() noexcept;

    // Call once per UI frame from the message thread.
    void serviceFromMessageThread();

    bool prepared() const noexcept { return prepared_.load(std::memory_order_acquire); }

private:
    Graph graph_;
    Transport transport_;

    AudioBuffer deviceInput_;
    AudioBuffer deviceOutput_;

    BlockCounter blockCounter_{ 0 };

    std::atomic<bool> prepared_{ false };
    std::atomic<bool> panicRequested_{ false };
    std::atomic<float> masterGainDb_{ 0.0f };
    std::atomic<float> masterGainLinear_{ 1.0f };
    std::atomic<bool> masterMuted_{ false };
    std::atomic<bool> limiter_{ true };
    std::atomic<float> masterPeak_[2] = {};

    std::atomic<float> cpuLoad_{ 0.0f };
    std::atomic<float> peakCpuLoad_{ 0.0f };
    std::atomic<int> xruns_{ 0 };
    std::atomic<int> lastBlockSize_{ 0 };

    SmoothedValue masterSmoothing_;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
};

} // namespace acm

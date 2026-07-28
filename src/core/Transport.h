// Musical timeline shared by the whole graph.
//
// Sample players and loopers sync to this rather than to each other, which is
// what makes "drop a loop in and it lands on the beat" work. The transport is
// advanced once per audio block by the engine; the UI reads a torn-but-harmless
// snapshot through atomics.
#pragma once

#include "Types.h"

#include <atomic>

namespace acm {

// The per-block view handed to nodes. Plain data, copied by value into the
// process context so a node can never observe the transport moving mid-block.
struct TransportState {
    bool playing = false;
    bool recording = false;

    double bpm = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;

    double sampleRate = 48000.0;
    std::int64_t samplePosition = 0;  // frames since the transport last started
    double ppqPosition = 0.0;         // quarter notes since zero
    double secondsPosition = 0.0;

    bool loopEnabled = false;
    double loopStartPpq = 0.0;
    double loopEndPpq = 16.0;

    // Derived, precomputed once per block so nodes don't each redo the maths.
    double samplesPerBeat = 24000.0;
    double samplesPerBar = 96000.0;

    double barPosition() const noexcept {
        const double beatsPerBar = static_cast<double>(timeSigNumerator) * 4.0
                                 / static_cast<double>(timeSigDenominator);
        return beatsPerBar > 0.0 ? ppqPosition / beatsPerBar : 0.0;
    }

    // 1-based bar and beat, as a performer would count them.
    int bar() const noexcept { return static_cast<int>(barPosition()) + 1; }
    int beatInBar() const noexcept {
        const double beatsPerBar = static_cast<double>(timeSigNumerator) * 4.0
                                 / static_cast<double>(timeSigDenominator);
        double b = ppqPosition;
        if (beatsPerBar > 0.0) b -= static_cast<double>(static_cast<int>(ppqPosition / beatsPerBar)) * beatsPerBar;
        return static_cast<int>(b) + 1;
    }

    double beatsToSamples(double beats) const noexcept { return beats * samplesPerBeat; }
    double samplesToBeats(double samples) const noexcept {
        return samplesPerBeat > 0.0 ? samples / samplesPerBeat : 0.0;
    }
};

class Transport {
public:
    void prepare(double sampleRate) noexcept;

    // -- audio thread ------------------------------------------------------

    // Fills `out` with the state at the start of the block, then moves the
    // timeline on by `frames`.
    void beginBlock(TransportState& out, int frames) noexcept;

    // -- any thread --------------------------------------------------------
    void setPlaying(bool p) noexcept;
    void togglePlaying() noexcept { setPlaying(!playing()); }
    bool playing() const noexcept { return playing_.load(std::memory_order_relaxed); }

    void setRecording(bool r) noexcept { recording_.store(r, std::memory_order_relaxed); }
    bool recording() const noexcept { return recording_.load(std::memory_order_relaxed); }

    void setBpm(double bpm) noexcept;
    double bpm() const noexcept { return bpm_.load(std::memory_order_relaxed); }

    void setTimeSignature(int numerator, int denominator) noexcept;
    int timeSigNumerator() const noexcept { return tsNum_.load(std::memory_order_relaxed); }
    int timeSigDenominator() const noexcept { return tsDen_.load(std::memory_order_relaxed); }

    void setLoop(bool enabled, double startPpq, double endPpq) noexcept;
    bool loopEnabled() const noexcept { return loopEnabled_.load(std::memory_order_relaxed); }
    double loopStartPpq() const noexcept { return loopStart_.load(std::memory_order_relaxed); }
    double loopEndPpq() const noexcept { return loopEnd_.load(std::memory_order_relaxed); }

    // Queues a rewind; applied at the top of the next block so the audio thread
    // never sees the position jump underneath it.
    void requestRewind() noexcept { rewindRequested_.store(true, std::memory_order_release); }

    double ppqPosition() const noexcept { return ppq_.load(std::memory_order_relaxed); }
    std::int64_t samplePosition() const noexcept { return samplePos_.load(std::memory_order_relaxed); }
    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }

    // Composes a state snapshot for display. Fields may be a block out of step
    // with each other; that is invisible at frame rate.
    TransportState snapshot() const noexcept;

    // -- tap tempo ---------------------------------------------------------
    // Feed successive tap times in seconds. Returns the newly derived BPM, or 0
    // when more taps are needed. A gap over two seconds restarts the average.
    double tap(double timeSeconds) noexcept;
    void resetTap() noexcept { tapCount_ = 0; }

private:
    std::atomic<bool> playing_{ false };
    std::atomic<bool> recording_{ false };
    std::atomic<double> bpm_{ 120.0 };
    std::atomic<int> tsNum_{ 4 };
    std::atomic<int> tsDen_{ 4 };
    std::atomic<double> sampleRate_{ 48000.0 };
    std::atomic<double> ppq_{ 0.0 };
    std::atomic<std::int64_t> samplePos_{ 0 };
    std::atomic<double> seconds_{ 0.0 };
    std::atomic<bool> loopEnabled_{ false };
    std::atomic<double> loopStart_{ 0.0 };
    std::atomic<double> loopEnd_{ 16.0 };
    std::atomic<bool> rewindRequested_{ false };

    // Tap tempo state - message thread only.
    static constexpr int kMaxTaps = 8;
    double tapTimes_[kMaxTaps] = {};
    int tapCount_ = 0;
};

} // namespace acm

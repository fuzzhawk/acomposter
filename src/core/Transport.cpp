#include "Transport.h"

#include <cmath>

namespace acm {

void Transport::prepare(double sampleRate) noexcept {
    sampleRate_.store(clampValue(sampleRate, kMinSampleRate, kMaxSampleRate), std::memory_order_relaxed);
    ppq_.store(0.0, std::memory_order_relaxed);
    samplePos_.store(0, std::memory_order_relaxed);
    seconds_.store(0.0, std::memory_order_relaxed);
}

void Transport::setPlaying(bool p) noexcept {
    playing_.store(p, std::memory_order_relaxed);
}

void Transport::setBpm(double bpm) noexcept {
    bpm_.store(clampValue(bpm, 20.0, 999.0), std::memory_order_relaxed);
}

void Transport::setTimeSignature(int numerator, int denominator) noexcept {
    tsNum_.store(clampValue(numerator, 1, 32), std::memory_order_relaxed);
    // Only the usual power-of-two note values make sense as a denominator.
    int d = clampValue(denominator, 1, 32);
    int pow2 = 1;
    while (pow2 * 2 <= d) pow2 *= 2;
    tsDen_.store(pow2, std::memory_order_relaxed);
}

void Transport::setLoop(bool enabled, double startPpq, double endPpq) noexcept {
    if (endPpq <= startPpq) endPpq = startPpq + 1.0;
    loopStart_.store(startPpq, std::memory_order_relaxed);
    loopEnd_.store(endPpq, std::memory_order_relaxed);
    loopEnabled_.store(enabled, std::memory_order_relaxed);
}

void Transport::beginBlock(TransportState& out, int frames) noexcept {
    if (rewindRequested_.exchange(false, std::memory_order_acquire)) {
        ppq_.store(loopEnabled() ? loopStartPpq() : 0.0, std::memory_order_relaxed);
        samplePos_.store(0, std::memory_order_relaxed);
        seconds_.store(0.0, std::memory_order_relaxed);
    }

    const double sr = sampleRate_.load(std::memory_order_relaxed);
    const double bpm = bpm_.load(std::memory_order_relaxed);
    const int tsNum = tsNum_.load(std::memory_order_relaxed);
    const int tsDen = tsDen_.load(std::memory_order_relaxed);
    const double samplesPerBeat = (bpm > 0.0) ? (60.0 / bpm) * sr : sr;
    const double beatsPerBar = static_cast<double>(tsNum) * 4.0 / static_cast<double>(tsDen);

    out.playing = playing_.load(std::memory_order_relaxed);
    out.recording = recording_.load(std::memory_order_relaxed);
    out.bpm = bpm;
    out.timeSigNumerator = tsNum;
    out.timeSigDenominator = tsDen;
    out.sampleRate = sr;
    out.samplePosition = samplePos_.load(std::memory_order_relaxed);
    out.ppqPosition = ppq_.load(std::memory_order_relaxed);
    out.secondsPosition = seconds_.load(std::memory_order_relaxed);
    out.loopEnabled = loopEnabled_.load(std::memory_order_relaxed);
    out.loopStartPpq = loopStart_.load(std::memory_order_relaxed);
    out.loopEndPpq = loopEnd_.load(std::memory_order_relaxed);
    out.samplesPerBeat = samplesPerBeat;
    out.samplesPerBar = samplesPerBeat * beatsPerBar;

    if (!out.playing || frames <= 0)
        return;

    // Advance. The transport is free-running within the block; nodes that need
    // sample-accurate loop wrapping compute it themselves from the start state.
    double ppq = out.ppqPosition + (samplesPerBeat > 0.0 ? static_cast<double>(frames) / samplesPerBeat : 0.0);

    if (out.loopEnabled) {
        const double span = out.loopEndPpq - out.loopStartPpq;
        if (span > 1.0e-9 && ppq >= out.loopEndPpq)
            ppq = out.loopStartPpq + std::fmod(ppq - out.loopStartPpq, span);
    }

    ppq_.store(ppq, std::memory_order_relaxed);
    samplePos_.store(out.samplePosition + frames, std::memory_order_relaxed);
    seconds_.store(out.secondsPosition + (sr > 0.0 ? static_cast<double>(frames) / sr : 0.0),
                   std::memory_order_relaxed);
}

TransportState Transport::snapshot() const noexcept {
    TransportState s;
    const double sr = sampleRate_.load(std::memory_order_relaxed);
    const double bpm = bpm_.load(std::memory_order_relaxed);
    const int tsNum = tsNum_.load(std::memory_order_relaxed);
    const int tsDen = tsDen_.load(std::memory_order_relaxed);

    s.playing = playing_.load(std::memory_order_relaxed);
    s.recording = recording_.load(std::memory_order_relaxed);
    s.bpm = bpm;
    s.timeSigNumerator = tsNum;
    s.timeSigDenominator = tsDen;
    s.sampleRate = sr;
    s.samplePosition = samplePos_.load(std::memory_order_relaxed);
    s.ppqPosition = ppq_.load(std::memory_order_relaxed);
    s.secondsPosition = seconds_.load(std::memory_order_relaxed);
    s.loopEnabled = loopEnabled_.load(std::memory_order_relaxed);
    s.loopStartPpq = loopStart_.load(std::memory_order_relaxed);
    s.loopEndPpq = loopEnd_.load(std::memory_order_relaxed);
    s.samplesPerBeat = (bpm > 0.0) ? (60.0 / bpm) * sr : sr;
    s.samplesPerBar = s.samplesPerBeat * (static_cast<double>(tsNum) * 4.0 / static_cast<double>(tsDen));
    return s;
}

double Transport::tap(double timeSeconds) noexcept {
    // A long gap means the performer stopped and started again.
    if (tapCount_ > 0 && timeSeconds - tapTimes_[tapCount_ - 1] > 2.0)
        tapCount_ = 0;

    if (tapCount_ == kMaxTaps) {
        for (int i = 1; i < kMaxTaps; ++i) tapTimes_[i - 1] = tapTimes_[i];
        --tapCount_;
    }
    tapTimes_[tapCount_++] = timeSeconds;

    if (tapCount_ < 2) return 0.0;

    // Average the intervals rather than using only the last one, so a single
    // sloppy tap does not throw the tempo off.
    const double span = tapTimes_[tapCount_ - 1] - tapTimes_[0];
    const double interval = span / static_cast<double>(tapCount_ - 1);
    if (interval <= 1.0e-4) return 0.0;

    const double bpm = clampValue(60.0 / interval, 20.0, 999.0);
    setBpm(bpm);
    return bpm;
}

} // namespace acm

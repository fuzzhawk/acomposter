// Small workhorse nodes: gain, oscillator, filter, and a metering probe.
#pragma once

#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>

namespace acm {

// Level, pan, polarity and DC removal in one box.
class GainNode : public Node {
public:
    GainNode();

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

private:
    ParamIndex pGain_ = -1, pPan_ = -1, pMute_ = -1, pInvert_ = -1, pDcBlock_ = -1;

    SmoothedValue gain_;
    SmoothedValue pan_;
    dsp::DcBlocker dcBlocker_[kMaxChannelsPerPort];
};

// Test and performance oscillator. The saw and square use polyBLEP correction,
// so this is usable as an actual sound source rather than only as a test signal.
class ToneNode : public Node {
public:
    ToneNode();

    enum class Waveform : int { Sine = 0, Triangle, Saw, Square, WhiteNoise, PinkNoise };

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

private:
    ParamIndex pWaveform_ = -1, pFrequency_ = -1, pLevel_ = -1, pPulseWidth_ = -1;
    ParamIndex pTuneToTempo_ = -1;

    double phase_ = 0.0;
    SmoothedValue level_;
    dsp::Xorshift noise_;
    // Paul Kellet's economy pink filter: three poles, good to about -3 dB/decade.
    float pinkState_[3] = {};
};

// State-variable-style biquad with a wet/dry blend.
class FilterNode : public Node {
public:
    FilterNode();

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

private:
    ParamIndex pType_ = -1, pFrequency_ = -1, pResonance_ = -1, pGainDb_ = -1, pMix_ = -1;

    dsp::Biquad filters_[kMaxChannelsPerPort];
    SmoothedValue mix_;

    // Cached so coefficients are only recomputed when something actually moved.
    float lastFrequency_ = -1.0f;
    float lastResonance_ = -1.0f;
    float lastGainDb_ = -1.0f;
    int lastType_ = -1;
};

// Passes audio through untouched and publishes peak and RMS for the UI. Useful
// for finding where a patch is losing level.
class MonitorNode : public Node {
public:
    MonitorNode();

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

    float peakLevel(int channel) const noexcept { return peak_[channel & 1].load(std::memory_order_relaxed); }
    float rmsLevel(int channel) const noexcept { return rms_[channel & 1].load(std::memory_order_relaxed); }
    // True if the signal has hit or exceeded full scale since the last reset.
    bool clipped() const noexcept { return clipped_.load(std::memory_order_relaxed); }
    void clearClip() noexcept { clipped_.store(false, std::memory_order_relaxed); }

private:
    dsp::PeakFollower peakFollower_[2];
    dsp::RmsFollower rmsFollower_[2];
    std::atomic<float> peak_[2] = {};
    std::atomic<float> rms_[2] = {};
    std::atomic<bool> clipped_{ false };
};

} // namespace acm

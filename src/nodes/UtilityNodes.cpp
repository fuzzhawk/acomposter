#include "UtilityNodes.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

constexpr double kTwoPi = 6.28318530717958647692;

// polyBLEP: subtracts the first-order discontinuity a naive saw or square
// introduces at each wrap, which removes most of the aliasing for a couple of
// multiplies per sample.
inline double polyBlep(double t, double dt) noexcept {
    if (dt <= 0.0) return 0.0;
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

} // namespace

// ---------------------------------------------------------------------------
// GainNode
// ---------------------------------------------------------------------------

GainNode::GainNode() : Node("util.gain", NodeCategory::Mixing) {
    setName("Gain");
    addInput("in", 2);
    addOutput("out", 2);

    addDbParam("gain", "Gain", -96.0f, 24.0f, 0.0f);
    pGain_ = indexOfParameter("gain");

    addFloatParam("pan", "Pan", -1.0f, 1.0f, 0.0f);
    pPan_ = indexOfParameter("pan");

    addBoolParam("mute", "Mute", false);
    pMute_ = indexOfParameter("mute");

    addBoolParam("invert", "Invert Polarity", false)
        .setDescription("Flips the phase. Handy for finding what is cancelling in a mix.");
    pInvert_ = indexOfParameter("invert");

    addBoolParam("dcblock", "DC Block", false)
        .setDescription("Removes DC offset, which feedback patches and some plugins accumulate.");
    pDcBlock_ = indexOfParameter("dcblock");
}

void GainNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pGain_)));
    pan_.reset(info.sampleRate, 0.02);
    pan_.setCurrentAndTarget(paramValue(pPan_));
    for (auto& b : dcBlocker_) b.prepare(info.sampleRate);
}

void GainNode::reset() {
    for (auto& b : dcBlocker_) b.reset();
}

void GainNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0 || ctx.numInputs == 0) return;

    AudioBus& in = ctx.input(0);
    AudioBus& out = ctx.output(0);

    const bool muted = paramValue(pMute_) >= 0.5f;
    const float polarity = paramValue(pInvert_) >= 0.5f ? -1.0f : 1.0f;
    const bool blockDc = paramValue(pDcBlock_) >= 0.5f;

    gain_.setTarget(muted ? 0.0f : dsp::dbToGain(paramValue(pGain_)));
    pan_.setTarget(paramValue(pPan_));

    const int channels = std::min(out.numChannels, kMaxChannelsPerPort);

    for (int i = 0; i < ctx.frames; ++i) {
        const float g = gain_.next() * polarity;
        float panLeft = 1.0f, panRight = 1.0f;
        dsp::panGains(pan_.next(), panLeft, panRight);

        for (int c = 0; c < channels; ++c) {
            float v = in.chan(c % in.numChannels)[i];
            if (blockDc) v = dcBlocker_[c].process(v);
            out.chan(c)[i] = v * g * (c == 0 ? panLeft : (c == 1 ? panRight : 1.0f));
        }
    }
}

// ---------------------------------------------------------------------------
// ToneNode
// ---------------------------------------------------------------------------

ToneNode::ToneNode() : Node("util.tone", NodeCategory::Source) {
    setName("Tone");
    addOutput("out", 2);

    addChoiceParam("waveform", "Waveform",
                   { "sine", "triangle", "saw", "square", "white noise", "pink noise" },
                   static_cast<int>(Waveform::Sine));
    pWaveform_ = indexOfParameter("waveform");

    addFloatParam("frequency", "Frequency", 20.0f, 20000.0f, 440.0f)
        .setUnit("Hz")
        .setSkewForCentre(1000.0f);
    pFrequency_ = indexOfParameter("frequency");

    addDbParam("level", "Level", -96.0f, 0.0f, -12.0f);
    pLevel_ = indexOfParameter("level");

    addFloatParam("pulsewidth", "Pulse Width", 0.02f, 0.98f, 0.5f)
        .setDescription("Duty cycle of the square wave.");
    pPulseWidth_ = indexOfParameter("pulsewidth");

    addBoolParam("tunetotempo", "Tune To Tempo", false)
        .setDescription("Locks the frequency to the transport tempo, for sub-audio "
                        "rate use as a modulation source.");
    pTuneToTempo_ = indexOfParameter("tunetotempo");
}

void ToneNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    level_.reset(info.sampleRate, 0.02);
    level_.setCurrentAndTarget(dsp::dbToGain(paramValue(pLevel_)));
    reset();
}

void ToneNode::reset() {
    phase_ = 0.0;
    for (auto& s : pinkState_) s = 0.0f;
}

void ToneNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    out.clear();

    const auto waveform = static_cast<Waveform>(static_cast<int>(paramValue(pWaveform_)));
    const float pulseWidth = clampValue(paramValue(pPulseWidth_), 0.02f, 0.98f);
    level_.setTarget(dsp::dbToGain(paramValue(pLevel_)));

    double frequency = static_cast<double>(paramValue(pFrequency_));
    if (paramValue(pTuneToTempo_) >= 0.5f && ctx.transport != nullptr) {
        // One cycle per beat, so the "frequency" knob becomes a beat multiplier.
        const double beatsPerSecond = ctx.transport->bpm / 60.0;
        frequency = beatsPerSecond * std::max(0.01, frequency / 440.0);
    }

    const double increment = frequency / (ctx.sampleRate > 0.0 ? ctx.sampleRate : 48000.0);

    for (int i = 0; i < ctx.frames; ++i) {
        const float amplitude = level_.next();
        double value = 0.0;

        switch (waveform) {
            case Waveform::Sine:
                value = std::sin(phase_ * kTwoPi);
                break;

            case Waveform::Triangle: {
                // Integrating a square would need DC tracking; the folded ramp is
                // exact and band-limited enough at these frequencies.
                const double t = phase_ < 0.5 ? phase_ * 2.0 : (1.0 - phase_) * 2.0;
                value = t * 2.0 - 1.0;
                break;
            }

            case Waveform::Saw:
                value = 2.0 * phase_ - 1.0;
                value -= polyBlep(phase_, increment);
                break;

            case Waveform::Square: {
                value = phase_ < pulseWidth ? 1.0 : -1.0;
                value += polyBlep(phase_, increment);
                // The second edge sits at the end of the duty cycle.
                double edge = phase_ - pulseWidth;
                if (edge < 0.0) edge += 1.0;
                value -= polyBlep(edge, increment);
                break;
            }

            case Waveform::WhiteNoise:
                value = noise_.nextFloat();
                break;

            case Waveform::PinkNoise: {
                const float white = noise_.nextFloat();
                pinkState_[0] = 0.99765f * pinkState_[0] + white * 0.0990460f;
                pinkState_[1] = 0.96300f * pinkState_[1] + white * 0.2965164f;
                pinkState_[2] = 0.57000f * pinkState_[2] + white * 1.0526913f;
                value = (pinkState_[0] + pinkState_[1] + pinkState_[2] + white * 0.1848f) * 0.25;
                break;
            }
        }

        const auto sample = static_cast<float>(value) * amplitude;
        for (int c = 0; c < out.numChannels; ++c) out.chan(c)[i] = sample;

        if (waveform != Waveform::WhiteNoise && waveform != Waveform::PinkNoise) {
            phase_ += increment;
            if (phase_ >= 1.0) phase_ -= std::floor(phase_);
        }
    }
}

// ---------------------------------------------------------------------------
// FilterNode
// ---------------------------------------------------------------------------

FilterNode::FilterNode() : Node("util.filter", NodeCategory::Effect) {
    setName("Filter");
    addInput("in", 2);
    addOutput("out", 2);

    addChoiceParam("type", "Type",
                   { "low pass", "high pass", "band pass", "notch", "peak",
                     "low shelf", "high shelf" }, 0);
    pType_ = indexOfParameter("type");

    addFloatParam("frequency", "Frequency", 20.0f, 20000.0f, 1000.0f)
        .setUnit("Hz")
        .setSkewForCentre(1000.0f);
    pFrequency_ = indexOfParameter("frequency");

    addFloatParam("resonance", "Resonance", 0.1f, 20.0f, 0.707f)
        .setSkewForCentre(1.0f)
        .setDescription("Q. 0.707 is flat; higher values ring.");
    pResonance_ = indexOfParameter("resonance");

    addFloatParam("gaindb", "Shelf/Peak Gain", -24.0f, 24.0f, 0.0f)
        .setUnit("dB")
        .setDescription("Only used by the peak and shelf types.");
    pGainDb_ = indexOfParameter("gaindb");

    addFloatParam("mix", "Mix", 0.0f, 1.0f, 1.0f);
    pMix_ = indexOfParameter("mix");
}

void FilterNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    mix_.reset(info.sampleRate, 0.02);
    mix_.setCurrentAndTarget(paramValue(pMix_));
    lastFrequency_ = lastResonance_ = lastGainDb_ = -1.0f;
    lastType_ = -1;
    reset();
}

void FilterNode::reset() {
    for (auto& f : filters_) f.reset();
}

void FilterNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0 || ctx.numInputs == 0) return;

    AudioBus& in = ctx.input(0);
    AudioBus& out = ctx.output(0);

    const int type = static_cast<int>(paramValue(pType_));
    const float frequency = paramValue(pFrequency_);
    const float resonance = paramValue(pResonance_);
    const float gainDb = paramValue(pGainDb_);

    // Coefficients are shared across channels and only recomputed on change; a
    // sweep from the metasurface still updates every block, which is smooth
    // enough at block sizes of a few milliseconds.
    if (type != lastType_ || frequency != lastFrequency_
        || resonance != lastResonance_ || gainDb != lastGainDb_) {
        const auto biquadType = static_cast<dsp::Biquad::Type>(clampValue(type, 0, 6));
        for (int c = 0; c < kMaxChannelsPerPort; ++c)
            filters_[c].setCoefficients(biquadType, ctx.sampleRate, frequency, resonance, gainDb);

        lastType_ = type;
        lastFrequency_ = frequency;
        lastResonance_ = resonance;
        lastGainDb_ = gainDb;
    }

    mix_.setTarget(clampValue(paramValue(pMix_), 0.0f, 1.0f));
    const int channels = std::min(out.numChannels, kMaxChannelsPerPort);

    for (int i = 0; i < ctx.frames; ++i) {
        const float wet = mix_.next();
        const float dry = 1.0f - wet;
        for (int c = 0; c < channels; ++c) {
            const float input = in.chan(c % in.numChannels)[i];
            out.chan(c)[i] = filters_[c].process(input) * wet + input * dry;
        }
    }
}

// ---------------------------------------------------------------------------
// MonitorNode
// ---------------------------------------------------------------------------

MonitorNode::MonitorNode() : Node("util.monitor", NodeCategory::Analysis) {
    setName("Monitor");
    addInput("in", 2);
    addOutput("thru", 2);
}

void MonitorNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    for (auto& f : peakFollower_) f.prepare(info.sampleRate);
    for (auto& f : rmsFollower_) f.prepare(info.sampleRate);
}

void MonitorNode::reset() {
    for (auto& f : peakFollower_) f.reset();
    for (auto& f : rmsFollower_) f.reset();
    for (auto& v : peak_) v.store(0.0f, std::memory_order_relaxed);
    for (auto& v : rms_) v.store(0.0f, std::memory_order_relaxed);
    clipped_.store(false, std::memory_order_relaxed);
}

void MonitorNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0 || ctx.numInputs == 0) return;

    AudioBus& in = ctx.input(0);
    AudioBus& out = ctx.output(0);

    for (int c = 0; c < out.numChannels; ++c) {
        const float* source = in.chan(c % in.numChannels);
        float* destination = out.chan(c);
        for (int i = 0; i < ctx.frames; ++i) destination[i] = source[i];
    }

    for (int c = 0; c < 2 && c < in.numChannels; ++c) {
        const float peak = peakFollower_[c].process(in.chan(c), ctx.frames);
        peak_[c].store(peak, std::memory_order_relaxed);
        rms_[c].store(rmsFollower_[c].process(in.chan(c), ctx.frames), std::memory_order_relaxed);
        // Latched, so a single overshoot during a set is still visible later.
        if (peak >= 0.999f) clipped_.store(true, std::memory_order_relaxed);
    }
}

} // namespace acm

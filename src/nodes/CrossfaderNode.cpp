#include "CrossfaderNode.h"

#include <algorithm>

namespace acm {

CrossfaderNode::CrossfaderNode() : Node("crossfader", NodeCategory::Mixing) {
    setName("Crossfader");
    addInput("A", 2);
    addInput("B", 2);
    addOutput("out", 2);

    addFloatParam("position", "Position", 0.0f, 1.0f, 0.5f)
        .setDescription("Fully on A at 0, fully on B at 1. The control the metasurface "
                        "will most often be asked to sweep.");
    pPosition_ = indexOfParameter("position");

    addChoiceParam("curve", "Curve",
                   { "constant power", "linear", "constant gain", "transition" },
                   static_cast<int>(dsp::FadeLaw::ConstantPower))
        .setDescription("Constant power holds perceived loudness across the sweep; "
                        "linear gives the classic dip; transition holds both sides up "
                        "and hands over late.");
    pCurve_ = indexOfParameter("curve");

    addDbParam("trima", "A Trim", -96.0f, 12.0f, 0.0f);
    pTrimA_ = indexOfParameter("trima");

    addDbParam("trimb", "B Trim", -96.0f, 12.0f, 0.0f);
    pTrimB_ = indexOfParameter("trimb");

    addBoolParam("cuta", "Cut A", false)
        .setDescription("Momentary kill for the A side, independent of the fader.");
    pCutA_ = indexOfParameter("cuta");

    addBoolParam("cutb", "Cut B", false);
    pCutB_ = indexOfParameter("cutb");

    addDbParam("gain", "Output", -96.0f, 12.0f, 0.0f);
    pGain_ = indexOfParameter("gain");
}

void CrossfaderNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);

    // The fader itself gets a shorter ramp than the trims: a performer expects a
    // cut to feel immediate, but still without a click.
    position_.reset(info.sampleRate, 0.008);
    position_.setCurrentAndTarget(paramValue(pPosition_));

    trimA_.reset(info.sampleRate, 0.02);
    trimA_.setCurrentAndTarget(dsp::dbToGain(paramValue(pTrimA_)));
    trimB_.reset(info.sampleRate, 0.02);
    trimB_.setCurrentAndTarget(dsp::dbToGain(paramValue(pTrimB_)));

    cutA_.reset(info.sampleRate, 0.004);
    cutA_.setCurrentAndTarget(1.0f);
    cutB_.reset(info.sampleRate, 0.004);
    cutB_.setCurrentAndTarget(1.0f);

    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pGain_)));

    for (auto& f : follower_) f.prepare(info.sampleRate);
}

void CrossfaderNode::reset() {
    for (auto& f : follower_) f.reset();
    for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
}

void CrossfaderNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    out.clear();
    if (ctx.numInputs < 2) return;

    AudioBus& a = ctx.input(0);
    AudioBus& b = ctx.input(1);

    const auto law = static_cast<dsp::FadeLaw>(static_cast<int>(paramValue(pCurve_)));

    position_.setTarget(clampValue(paramValue(pPosition_), 0.0f, 1.0f));
    trimA_.setTarget(dsp::dbToGain(paramValue(pTrimA_)));
    trimB_.setTarget(dsp::dbToGain(paramValue(pTrimB_)));
    cutA_.setTarget(paramValue(pCutA_) >= 0.5f ? 0.0f : 1.0f);
    cutB_.setTarget(paramValue(pCutB_) >= 0.5f ? 0.0f : 1.0f);
    gain_.setTarget(dsp::dbToGain(paramValue(pGain_)));

    const int channels = out.numChannels;
    float lastGainA = 0.0f, lastGainB = 0.0f;

    for (int i = 0; i < ctx.frames; ++i) {
        const float t = position_.next();

        float gainA = 0.0f, gainB = 0.0f;
        dsp::crossfadeGains(law, t, gainA, gainB);

        gainA *= trimA_.next() * cutA_.next();
        gainB *= trimB_.next() * cutB_.next();

        const float outGain = gain_.next();
        lastGainA = gainA;
        lastGainB = gainB;

        for (int c = 0; c < channels; ++c) {
            const float sampleA = a.connected ? a.chan(c % a.numChannels)[i] : 0.0f;
            const float sampleB = b.connected ? b.chan(c % b.numChannels)[i] : 0.0f;
            out.chan(c)[i] = (sampleA * gainA + sampleB * gainB) * outGain;
        }
    }

    uiPosition_.store(position_.current(), std::memory_order_relaxed);
    uiSideGain_[0].store(lastGainA, std::memory_order_relaxed);
    uiSideGain_[1].store(lastGainB, std::memory_order_relaxed);

    for (int c = 0; c < 2 && c < channels; ++c)
        meter_[c].store(follower_[c].process(out.chan(c), ctx.frames), std::memory_order_relaxed);
}

} // namespace acm

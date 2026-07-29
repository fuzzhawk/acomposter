#include "IoNodes.h"

#include <algorithm>

namespace acm {

// ---------------------------------------------------------------------------
// AudioOutNode
// ---------------------------------------------------------------------------

AudioOutNode::AudioOutNode() : Node("io.out", NodeCategory::Output) {
    setName("Audio Out");
    addInput("in", 2);

    addDbParam("gain", "Gain", -96.0f, 12.0f, 0.0f)
        .setDescription("Level applied before the signal reaches the output device.");
    gainParam_ = indexOfParameter("gain");

    addBoolParam("mute", "Mute", false);
    muteParam_ = indexOfParameter("mute");
}

void AudioOutNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(gainParam_)));
    for (auto& f : follower_) f.prepare(info.sampleRate);
}

void AudioOutNode::reset() {
    for (auto& f : follower_) f.reset();
    for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
}

void AudioOutNode::process(ProcessContext& ctx) {
    AudioBuffer* destination = ctx.deviceOutput;
    if (destination == nullptr || ctx.numInputs == 0) return;

    const bool muted = paramValue(muteParam_) >= 0.5f;
    gain_.setTarget(muted ? 0.0f : dsp::dbToGain(paramValue(gainParam_)));

    AudioBus& in = ctx.input(0);
    const int channels = std::min(destination->channels(), in.numChannels);

    // One gain ramp shared across channels keeps the stereo image intact while
    // the fader moves.
    for (int i = 0; i < ctx.frames; ++i) {
        const float g = gain_.next();
        for (int c = 0; c < channels; ++c)
            destination->channel(c)[i] += in.chan(c)[i] * g;
    }

    for (int c = 0; c < 2 && c < in.numChannels; ++c)
        meter_[c].store(follower_[c].process(in.chan(c), ctx.frames), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// AudioInNode
// ---------------------------------------------------------------------------

AudioInNode::AudioInNode() : Node("io.in", NodeCategory::Source) {
    setName("Audio In");
    addOutput("out", 2);

    addDbParam("gain", "Gain", -96.0f, 24.0f, 0.0f)
        .setDescription("Input trim applied as the capture stream enters the patch.");
    gainParam_ = indexOfParameter("gain");

    addBoolParam("mute", "Mute", false);
    muteParam_ = indexOfParameter("mute");
}

void AudioInNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(gainParam_)));
    for (auto& f : follower_) f.prepare(info.sampleRate);
}

void AudioInNode::reset() {
    for (auto& f : follower_) f.reset();
    for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
}

void AudioInNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    const AudioBuffer* source = ctx.deviceInput;

    if (source == nullptr || source->channels() == 0) {
        // No capture stream configured: emit silence rather than stale audio.
        out.clear();
        for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const bool muted = paramValue(muteParam_) >= 0.5f;
    gain_.setTarget(muted ? 0.0f : dsp::dbToGain(paramValue(gainParam_)));

    const int sourceChannels = source->channels();

    for (int i = 0; i < ctx.frames; ++i) {
        const float g = gain_.next();
        for (int c = 0; c < out.numChannels; ++c) {
            // A mono interface feeding a stereo patch should be heard in both
            // sides, not just the left.
            out.chan(c)[i] = source->channel(c % sourceChannels)[i] * g;
        }
    }

    for (int c = 0; c < 2 && c < out.numChannels; ++c)
        meter_[c].store(follower_[c].process(out.chan(c), ctx.frames), std::memory_order_relaxed);
}

} // namespace acm

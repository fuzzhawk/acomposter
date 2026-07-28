#include "MixerNode.h"

#include <algorithm>
#include <string>

namespace acm {

std::string MixerNode::typeNameForWidth(int channelCount) {
    return "mixer." + std::to_string(channelCount);
}

MixerNode::MixerNode(int channelCount)
    : Node(typeNameForWidth(clampValue(channelCount, 2, 16)), NodeCategory::Mixing),
      channelCount_(clampValue(channelCount, 2, 16)) {
    setName("Mixer " + std::to_string(channelCount_));

    for (int i = 0; i < channelCount_; ++i)
        addInput(std::to_string(i + 1), 2);

    addOutput("out", 2);

    strips_.reserve(static_cast<std::size_t>(channelCount_));

    for (int i = 0; i < channelCount_; ++i) {
        auto strip = std::make_unique<Strip>();
        const std::string suffix = std::to_string(i + 1);

        addDbParam("gain" + suffix, "Ch " + suffix + " Gain", -96.0f, 12.0f, 0.0f);
        strip->gain = indexOfParameter("gain" + suffix);

        addFloatParam("pan" + suffix, "Ch " + suffix + " Pan", -1.0f, 1.0f, 0.0f);
        strip->pan = indexOfParameter("pan" + suffix);

        addBoolParam("mute" + suffix, "Ch " + suffix + " Mute", false);
        strip->mute = indexOfParameter("mute" + suffix);

        addBoolParam("solo" + suffix, "Ch " + suffix + " Solo", false);
        strip->solo = indexOfParameter("solo" + suffix);

        strips_.push_back(std::move(strip));
    }

    addDbParam("mastergain", "Master", -96.0f, 12.0f, 0.0f);
    pMasterGain_ = indexOfParameter("mastergain");

    addBoolParam("mastermute", "Master Mute", false);
    pMasterMute_ = indexOfParameter("mastermute");
}

float MixerNode::channelMeter(int channel, int side) const noexcept {
    if (channel < 0 || channel >= channelCount_) return 0.0f;
    return strips_[static_cast<std::size_t>(channel)]->meter[side & 1].load(std::memory_order_relaxed);
}

bool MixerNode::channelSilencedBySolo(int channel) const {
    bool anySolo = false;
    for (const auto& strip : strips_) {
        if (paramValue(strip->solo) >= 0.5f) { anySolo = true; break; }
    }
    if (!anySolo) return false;
    if (channel < 0 || channel >= channelCount_) return false;
    return paramValue(strips_[static_cast<std::size_t>(channel)]->solo) < 0.5f;
}

void MixerNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);

    for (auto& strip : strips_) {
        strip->gainSmoother.reset(info.sampleRate, 0.02);
        strip->gainSmoother.setCurrentAndTarget(dsp::dbToGain(paramValue(strip->gain)));
        strip->panSmoother.reset(info.sampleRate, 0.02);
        strip->panSmoother.setCurrentAndTarget(paramValue(strip->pan));
        for (auto& f : strip->follower) f.prepare(info.sampleRate);
    }

    masterGain_.reset(info.sampleRate, 0.02);
    masterGain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pMasterGain_)));
    for (auto& f : masterFollower_) f.prepare(info.sampleRate);
}

void MixerNode::reset() {
    for (auto& strip : strips_) {
        for (auto& f : strip->follower) f.reset();
        for (auto& m : strip->meter) m.store(0.0f, std::memory_order_relaxed);
    }
    for (auto& f : masterFollower_) f.reset();
    for (auto& m : masterMeter_) m.store(0.0f, std::memory_order_relaxed);
}

void MixerNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    out.clear();

    // Solo is exclusive across the whole mixer, evaluated once per block.
    bool anySolo = false;
    for (const auto& strip : strips_)
        if (paramValue(strip->solo) >= 0.5f) { anySolo = true; break; }

    const int busChannels = out.numChannels;

    for (int channel = 0; channel < channelCount_ && channel < ctx.numInputs; ++channel) {
        Strip& strip = *strips_[static_cast<std::size_t>(channel)];
        AudioBus& in = ctx.input(channel);

        const bool muted = paramValue(strip.mute) >= 0.5f;
        const bool soloed = paramValue(strip.solo) >= 0.5f;
        const bool audible = !muted && (!anySolo || soloed);

        strip.gainSmoother.setTarget(audible ? dsp::dbToGain(paramValue(strip.gain)) : 0.0f);
        strip.panSmoother.setTarget(paramValue(strip.pan));

        // A channel with nothing patched in still runs its smoothers, so that
        // patching something in later does not produce a jump.
        if (!in.connected) {
            for (int i = 0; i < ctx.frames; ++i) { strip.gainSmoother.next(); strip.panSmoother.next(); }
            for (auto& m : strip.meter) m.store(0.0f, std::memory_order_relaxed);
            continue;
        }

        for (int i = 0; i < ctx.frames; ++i) {
            const float g = strip.gainSmoother.next();
            float panLeft = 1.0f, panRight = 1.0f;
            dsp::panGains(strip.panSmoother.next(), panLeft, panRight);

            for (int c = 0; c < busChannels; ++c) {
                const float sample = in.chan(c % in.numChannels)[i];
                const float panGain = (c == 0) ? panLeft : (c == 1 ? panRight : 1.0f);
                out.chan(c)[i] += sample * g * panGain;
            }
        }

        // Meter the channel pre-fader-post-mute so a muted strip reads silent.
        for (int c = 0; c < 2 && c < in.numChannels; ++c)
            strip.meter[c].store(strip.follower[c].process(in.chan(c), ctx.frames) *
                                     (audible ? 1.0f : 0.0f),
                                 std::memory_order_relaxed);
    }

    const bool masterMuted = paramValue(pMasterMute_) >= 0.5f;
    masterGain_.setTarget(masterMuted ? 0.0f : dsp::dbToGain(paramValue(pMasterGain_)));

    for (int i = 0; i < ctx.frames; ++i) {
        const float g = masterGain_.next();
        for (int c = 0; c < busChannels; ++c) out.chan(c)[i] *= g;
    }

    for (int c = 0; c < 2 && c < busChannels; ++c)
        masterMeter_[c].store(masterFollower_[c].process(out.chan(c), ctx.frames),
                              std::memory_order_relaxed);
}

} // namespace acm

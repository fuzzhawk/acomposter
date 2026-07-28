// Multi-channel mixer with per-channel gain, pan, mute and solo.
//
// The channel count is fixed at construction because ports are declared once,
// so the factory registers a few sizes ("mixer.4", "mixer.8", "mixer.16") and
// the type name carries the width through a save/load round trip.
#pragma once

#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>
#include <memory>
#include <vector>

namespace acm {

class MixerNode : public Node {
public:
    explicit MixerNode(int channelCount);

    static std::string typeNameForWidth(int channelCount);

    int channelCount() const noexcept { return channelCount_; }

    // -- readouts for the UI -----------------------------------------------
    float channelMeter(int channel, int side) const noexcept;
    float masterMeter(int side) const noexcept {
        return masterMeter_[side & 1].load(std::memory_order_relaxed);
    }
    // True when some other channel's solo is silencing this one.
    bool channelSilencedBySolo(int channel) const;

    ParamIndex gainParam(int channel) const { return strips_[static_cast<std::size_t>(channel)]->gain; }
    ParamIndex panParam(int channel) const { return strips_[static_cast<std::size_t>(channel)]->pan; }
    ParamIndex muteParam(int channel) const { return strips_[static_cast<std::size_t>(channel)]->mute; }
    ParamIndex soloParam(int channel) const { return strips_[static_cast<std::size_t>(channel)]->solo; }

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

private:
    struct Strip {
        ParamIndex gain = -1;
        ParamIndex pan = -1;
        ParamIndex mute = -1;
        ParamIndex solo = -1;

        SmoothedValue gainSmoother;
        SmoothedValue panSmoother;
        dsp::PeakFollower follower[2];
        std::atomic<float> meter[2] = {};
    };

    int channelCount_ = 4;
    // unique_ptr because Strip holds atomics and so cannot be moved by vector.
    std::vector<std::unique_ptr<Strip>> strips_;

    ParamIndex pMasterGain_ = -1;
    ParamIndex pMasterMute_ = -1;

    SmoothedValue masterGain_;
    dsp::PeakFollower masterFollower_[2];
    std::atomic<float> masterMeter_[2] = {};
};

} // namespace acm

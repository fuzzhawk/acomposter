// Two-source crossfader.
//
// The fader position is the single most performed control in this whole
// application, so it gets its own node with selectable curve laws and hard cut
// buttons for each side rather than being buried in the mixer.
#pragma once

#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>

namespace acm {

class CrossfaderNode : public Node {
public:
    CrossfaderNode();

    // -- readouts for the UI -----------------------------------------------
    float currentPosition() const noexcept { return uiPosition_.load(std::memory_order_relaxed); }
    float sideGain(int side) const noexcept {
        return uiSideGain_[side & 1].load(std::memory_order_relaxed);
    }
    float meterLevel(int channel) const noexcept {
        return meter_[channel & 1].load(std::memory_order_relaxed);
    }

    ParamIndex positionParam() const noexcept { return pPosition_; }

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

private:
    ParamIndex pPosition_ = -1, pCurve_ = -1, pTrimA_ = -1, pTrimB_ = -1;
    ParamIndex pCutA_ = -1, pCutB_ = -1, pGain_ = -1;

    SmoothedValue position_;
    SmoothedValue trimA_;
    SmoothedValue trimB_;
    SmoothedValue cutA_;
    SmoothedValue cutB_;
    SmoothedValue gain_;

    dsp::PeakFollower follower_[2];
    std::atomic<float> meter_[2] = {};
    std::atomic<float> uiPosition_{ 0.5f };
    std::atomic<float> uiSideGain_[2] = {};
};

} // namespace acm

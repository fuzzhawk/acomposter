// The hardware edges of the patch.
//
// Neither node touches the device directly: the engine puts the capture buffer
// and the playback buffer on the process context, and these two are the only
// things that read or write them.
#pragma once

#include "../core/Node.h"
#include "../dsp/Dsp.h"

namespace acm {

// Sums whatever is patched into it onto the master output bus. A patch can have
// more than one; they add.
class AudioOutNode : public Node {
public:
    AudioOutNode();

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

    float meterLevel(int channel) const noexcept {
        return meter_[channel & 1].load(std::memory_order_relaxed);
    }

private:
    ParamIndex gainParam_ = -1;
    ParamIndex muteParam_ = -1;

    SmoothedValue gain_;
    dsp::PeakFollower follower_[2];
    std::atomic<float> meter_[2] = {};
};

// Brings the capture stream into the patch.
class AudioInNode : public Node {
public:
    AudioInNode();

    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;

    float meterLevel(int channel) const noexcept {
        return meter_[channel & 1].load(std::memory_order_relaxed);
    }

private:
    ParamIndex gainParam_ = -1;
    ParamIndex muteParam_ = -1;

    SmoothedValue gain_;
    dsp::PeakFollower follower_[2];
    std::atomic<float> meter_[2] = {};
};

} // namespace acm

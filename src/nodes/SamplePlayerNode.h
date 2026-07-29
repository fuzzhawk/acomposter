// Sample player and loop deck.
//
// Two playback engines share one node:
//
//   Free   - a phase accumulator advanced by speed x pitch. What you want for
//            one-shots, stabs, and deliberately drifting layers.
//   Locked - the read position is derived directly from the transport's musical
//            position every sample, so the loop is squeezed to a whole number of
//            beats and can never drift, no matter how long the set runs. This is
//            the mode that makes dropping a break into a patch just work.
//
// The loaded sample is immutable and handed to the audio thread through an
// AtomicResource, so loading a new file while the patch is running is a pointer
// store rather than a lock.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/AtomicResource.h"
#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>
#include <memory>
#include <string>

namespace acm {

class SamplePlayerNode : public Node {
public:
    SamplePlayerNode();

    // -- sample management (message thread) --------------------------------
    bool loadFile(const std::string& utf8Path, std::string* error = nullptr);
    void setSample(std::shared_ptr<SampleBuffer> sample);
    void clearSample();

    // Safe to call from the UI; null when nothing is loaded.
    std::shared_ptr<SampleBuffer> sample() const { return sample_.shared(); }
    const std::string& samplePath() const noexcept { return samplePath_; }

    // -- performance control -----------------------------------------------
    // Restarts from the loop start, honouring the trigger quantise setting.
    void trigger() noexcept { triggerRequested_.store(true, std::memory_order_release); }
    void stop() noexcept;

    // -- readouts for the UI -----------------------------------------------
    double playPositionFrames() const noexcept { return uiPosition_.load(std::memory_order_relaxed); }
    // 0..1 across the whole file, for the playhead on the waveform strip.
    float playPositionNormalised() const noexcept;
    bool sounding() const noexcept { return uiSounding_.load(std::memory_order_relaxed); }
    float meterLevel(int channel) const noexcept {
        return meter_[channel & 1].load(std::memory_order_relaxed);
    }

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

    // Choice orders, referenced by the inline editor in the patcher.
    enum class Mode : int { Loop = 0, OneShot, Gate };
    enum class Sync : int { Free = 0, Locked };
    enum class Direction : int { Forward = 0, Reverse, PingPong };
    enum class Quantise : int { Off = 0, Beat, Bar };

private:
    // Resolves the loop region in frames from the start/end parameters.
    void resolveLoopRegion(const SampleBuffer& s, double& startFrames, double& endFrames) const;
    float readSample(const SampleBuffer& s, int channel, double position) const;

    AtomicResource<SampleBuffer> sample_;
    std::string samplePath_;

    // Parameter indices, cached to keep process() out of string comparisons.
    ParamIndex pGain_ = -1, pPan_ = -1, pPlay_ = -1, pMode_ = -1, pSync_ = -1;
    ParamIndex pLoopBeats_ = -1, pSpeed_ = -1, pPitch_ = -1, pStart_ = -1, pEnd_ = -1;
    ParamIndex pDirection_ = -1, pCrossfade_ = -1, pQuantise_ = -1, pFollowTransport_ = -1;

    // Audio-thread playback state.
    double position_ = 0.0;
    bool forward_ = true;
    bool sounding_ = false;
    bool pendingStart_ = false;
    bool lastPlayFlag_ = false;
    double lastPpq_ = 0.0;

    SmoothedValue gain_;
    SmoothedValue pan_;
    SmoothedValue envelope_;   // click-free start and stop

    dsp::PeakFollower follower_[2];

    std::atomic<bool> triggerRequested_{ false };
    std::atomic<bool> stopRequested_{ false };
    std::atomic<double> uiPosition_{ 0.0 };
    std::atomic<bool> uiSounding_{ false };
    std::atomic<float> meter_[2] = {};
    std::atomic<std::int64_t> uiSampleFrames_{ 0 };
};

} // namespace acm

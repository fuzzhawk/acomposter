// Live looper with overdub.
//
// The recording buffer is allocated once at prepare() and never resized, so
// arming, recording, overdubbing and clearing are all allocation-free. The first
// pass defines the loop length; if a beat length is set, that first pass is
// rounded to the grid so a slightly early or late punch still lands in time.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>
#include <memory>
#include <vector>

namespace acm {

class LooperNode : public Node {
public:
    LooperNode();

    enum class State : int { Empty = 0, Armed, Recording, Playing, Overdubbing, Stopped };

    // -- performance control (any thread) ----------------------------------
    // The one-button workflow: empty -> record -> play -> overdub -> play ...
    void toggleRecord() noexcept { recordToggle_.store(true, std::memory_order_release); }
    void toggleOverdub() noexcept { overdubToggle_.store(true, std::memory_order_release); }
    void togglePlay() noexcept { playToggle_.store(true, std::memory_order_release); }
    void clear() noexcept { clearRequested_.store(true, std::memory_order_release); }
    // Halves or doubles the loop without re-recording it.
    void halveLength() noexcept { halveRequested_.store(true, std::memory_order_release); }
    void doubleLength() noexcept { doubleRequested_.store(true, std::memory_order_release); }

    // -- readouts for the UI -----------------------------------------------
    State state() const noexcept { return static_cast<State>(uiState_.load(std::memory_order_relaxed)); }
    const char* stateName() const noexcept;
    float positionNormalised() const noexcept;
    double loopSeconds() const noexcept;
    std::int64_t loopFrames() const noexcept { return uiLoopFrames_.load(std::memory_order_relaxed); }
    float meterLevel(int channel) const noexcept {
        return meter_[channel & 1].load(std::memory_order_relaxed);
    }
    // Coarse envelope of the recorded take, refreshed on the message thread.
    const std::vector<float>& overview() const noexcept { return overview_; }

    // Snapshot of the current take, for dragging into a sample player or saving
    // to disk. Message thread only.
    std::shared_ptr<SampleBuffer> captureTake() const;

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

private:
    void applyTransition(const TransportState& transport, int frames);
    void setState(State s) noexcept;
    std::int64_t quantiseLength(std::int64_t frames, const TransportState& transport) const;

    // Recording storage: planar, capacity frames per channel.
    std::vector<float> storage_;
    int channels_ = 2;
    std::int64_t capacity_ = 0;

    float* channelData(int c) noexcept {
        return storage_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(capacity_);
    }
    const float* channelData(int c) const noexcept {
        return storage_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(capacity_);
    }

    ParamIndex pGain_ = -1, pFeedback_ = -1, pMonitor_ = -1, pSyncBeats_ = -1;
    ParamIndex pSpeed_ = -1, pReverse_ = -1, pInputGain_ = -1, pFadeMs_ = -1;

    State state_ = State::Empty;
    std::int64_t recordedFrames_ = 0;   // length of the take
    std::int64_t writeHead_ = 0;
    double readHead_ = 0.0;

    SmoothedValue gain_;
    SmoothedValue inputGain_;
    SmoothedValue recordEnvelope_;   // fades the punch in and out

    dsp::PeakFollower follower_[2];

    std::atomic<bool> recordToggle_{ false };
    std::atomic<bool> overdubToggle_{ false };
    std::atomic<bool> playToggle_{ false };
    std::atomic<bool> clearRequested_{ false };
    std::atomic<bool> halveRequested_{ false };
    std::atomic<bool> doubleRequested_{ false };

    std::atomic<int> uiState_{ 0 };
    std::atomic<double> uiPosition_{ 0.0 };
    std::atomic<std::int64_t> uiLoopFrames_{ 0 };
    std::atomic<bool> overviewDirty_{ false };
    std::atomic<float> meter_[2] = {};

    std::vector<float> overview_;
    double sampleRate_ = 48000.0;
};

} // namespace acm

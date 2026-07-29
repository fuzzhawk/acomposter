#include "LooperNode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace acm {
namespace {

// Two minutes of stereo at 48 kHz is about 46 MB. Generous for a performance
// looper and still small enough to allocate once without thinking about it.
constexpr double kMaxLoopSeconds = 120.0;
constexpr int kOverviewBuckets = 512;

} // namespace

LooperNode::LooperNode() : Node("looper", NodeCategory::Source) {
    setName("Looper");
    addInput("in", 2);
    addOutput("out", 2);

    addDbParam("gain", "Gain", -96.0f, 12.0f, 0.0f);
    pGain_ = indexOfParameter("gain");

    addDbParam("inputgain", "Input Gain", -96.0f, 24.0f, 0.0f)
        .setDescription("Trim applied to incoming audio before it is recorded.");
    pInputGain_ = indexOfParameter("inputgain");

    addFloatParam("feedback", "Feedback", 0.0f, 1.0f, 1.0f)
        .setDescription("How much of the existing take survives each overdub pass. "
                        "Below 1 the loop decays as you layer.");
    pFeedback_ = indexOfParameter("feedback");

    addBoolParam("monitor", "Monitor", true)
        .setDescription("Passes the live input through to the output as well as recording it.");
    pMonitor_ = indexOfParameter("monitor");

    addFloatParam("syncbeats", "Sync Length", 0.0f, 64.0f, 0.0f)
        .setUnit("beats")
        .setDescription("Rounds the recorded length to this many beats. Zero records freely.");
    pSyncBeats_ = indexOfParameter("syncbeats");

    addFloatParam("speed", "Speed", 0.25f, 4.0f, 1.0f)
        .setSkewForCentre(1.0f)
        .setUnit("x");
    pSpeed_ = indexOfParameter("speed");

    addBoolParam("reverse", "Reverse", false);
    pReverse_ = indexOfParameter("reverse");

    addFloatParam("punchfade", "Punch Fade", 0.0f, 100.0f, 6.0f)
        .setUnit("ms")
        .setDescription("Fade applied when recording starts and stops, so punches do not click.");
    pFadeMs_ = indexOfParameter("punchfade");
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LooperNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    sampleRate_ = info.sampleRate;

    channels_ = 2;
    capacity_ = static_cast<std::int64_t>(kMaxLoopSeconds * info.sampleRate);
    storage_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_), 0.0f);

    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pGain_)));
    inputGain_.reset(info.sampleRate, 0.02);
    inputGain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pInputGain_)));
    recordEnvelope_.reset(info.sampleRate, 0.006);
    recordEnvelope_.setCurrentAndTarget(0.0f);

    for (auto& f : follower_) f.prepare(info.sampleRate);

    overview_.assign(kOverviewBuckets, 0.0f);
    reset();
}

void LooperNode::reset() {
    state_ = recordedFrames_ > 0 ? State::Stopped : State::Empty;
    writeHead_ = 0;
    readHead_ = 0.0;
    recordEnvelope_.setCurrentAndTarget(0.0f);
    for (auto& f : follower_) f.reset();
    setState(state_);
}

void LooperNode::setState(State s) noexcept {
    state_ = s;
    uiState_.store(static_cast<int>(s), std::memory_order_relaxed);
}

const char* LooperNode::stateName() const noexcept {
    switch (state()) {
        case State::Empty:      return "empty";
        case State::Armed:      return "armed";
        case State::Recording:  return "recording";
        case State::Playing:    return "playing";
        case State::Overdubbing:return "overdub";
        case State::Stopped:    return "stopped";
    }
    return "?";
}

float LooperNode::positionNormalised() const noexcept {
    const auto frames = uiLoopFrames_.load(std::memory_order_relaxed);
    if (frames <= 0) return 0.0f;
    return clampValue(static_cast<float>(uiPosition_.load(std::memory_order_relaxed)
                                       / static_cast<double>(frames)), 0.0f, 1.0f);
}

double LooperNode::loopSeconds() const noexcept {
    const auto frames = uiLoopFrames_.load(std::memory_order_relaxed);
    return sampleRate_ > 0.0 ? static_cast<double>(frames) / sampleRate_ : 0.0;
}

// ---------------------------------------------------------------------------
// Transitions
// ---------------------------------------------------------------------------

std::int64_t LooperNode::quantiseLength(std::int64_t frames, const TransportState& transport) const {
    const double beats = static_cast<double>(paramValue(pSyncBeats_));
    if (beats <= 0.0 || transport.samplesPerBeat <= 0.0) return frames;

    const double gridFrames = beats * transport.samplesPerBeat;
    if (gridFrames < 1.0) return frames;

    // Round to the nearest whole multiple, but never collapse to zero: punching
    // out fractionally early should still give you one full cycle.
    auto multiples = static_cast<std::int64_t>(std::llround(static_cast<double>(frames) / gridFrames));
    if (multiples < 1) multiples = 1;

    return static_cast<std::int64_t>(static_cast<double>(multiples) * gridFrames);
}

void LooperNode::applyTransition(const TransportState& transport, int frames) {
    if (clearRequested_.exchange(false, std::memory_order_acquire)) {
        std::fill(storage_.begin(), storage_.end(), 0.0f);
        recordedFrames_ = 0;
        writeHead_ = 0;
        readHead_ = 0.0;
        recordEnvelope_.setCurrentAndTarget(0.0f);
        uiLoopFrames_.store(0, std::memory_order_relaxed);
        overviewDirty_.store(true, std::memory_order_release);
        setState(State::Empty);
    }

    if (recordToggle_.exchange(false, std::memory_order_acquire)) {
        switch (state_) {
            case State::Empty:
            case State::Stopped:
                // Starting a fresh take discards whatever was there.
                if (state_ == State::Stopped) {
                    std::fill(storage_.begin(), storage_.end(), 0.0f);
                    recordedFrames_ = 0;
                }
                writeHead_ = 0;
                readHead_ = 0.0;
                recordEnvelope_.setTarget(1.0f);
                setState(State::Recording);
                break;

            case State::Recording: {
                // Punching out defines the loop.
                recordedFrames_ = quantiseLength(writeHead_, transport);
                if (recordedFrames_ > capacity_) recordedFrames_ = capacity_;
                if (recordedFrames_ < 16) {
                    setState(State::Empty);
                    recordedFrames_ = 0;
                } else {
                    // A quantised length can exceed what was actually recorded;
                    // the tail is silence, which is the musically correct result.
                    readHead_ = 0.0;
                    recordEnvelope_.setTarget(0.0f);
                    setState(State::Playing);
                }
                uiLoopFrames_.store(recordedFrames_, std::memory_order_relaxed);
                overviewDirty_.store(true, std::memory_order_release);
                break;
            }

            case State::Playing:
            case State::Overdubbing:
            case State::Armed:
                recordEnvelope_.setTarget(0.0f);
                setState(State::Playing);
                break;
        }
    }

    if (overdubToggle_.exchange(false, std::memory_order_acquire)) {
        if (state_ == State::Playing) {
            recordEnvelope_.setTarget(1.0f);
            setState(State::Overdubbing);
        } else if (state_ == State::Overdubbing) {
            recordEnvelope_.setTarget(0.0f);
            setState(State::Playing);
            overviewDirty_.store(true, std::memory_order_release);
        }
    }

    if (playToggle_.exchange(false, std::memory_order_acquire)) {
        if (state_ == State::Playing || state_ == State::Overdubbing) {
            recordEnvelope_.setTarget(0.0f);
            setState(State::Stopped);
        } else if (recordedFrames_ > 0) {
            readHead_ = 0.0;
            setState(State::Playing);
        }
    }

    if (halveRequested_.exchange(false, std::memory_order_acquire) && recordedFrames_ >= 32) {
        recordedFrames_ /= 2;
        if (readHead_ >= static_cast<double>(recordedFrames_)) readHead_ = 0.0;
        uiLoopFrames_.store(recordedFrames_, std::memory_order_relaxed);
        overviewDirty_.store(true, std::memory_order_release);
    }

    if (doubleRequested_.exchange(false, std::memory_order_acquire)
        && recordedFrames_ > 0 && recordedFrames_ * 2 <= capacity_) {
        // Copy the take after itself so the doubled loop repeats rather than
        // running into silence.
        for (int c = 0; c < channels_; ++c) {
            float* data = channelData(c);
            std::memcpy(data + recordedFrames_, data,
                        sizeof(float) * static_cast<std::size_t>(recordedFrames_));
        }
        recordedFrames_ *= 2;
        uiLoopFrames_.store(recordedFrames_, std::memory_order_relaxed);
        overviewDirty_.store(true, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void LooperNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0 || capacity_ <= 0) return;

    AudioBus& out = ctx.output(0);
    out.clear();

    const TransportState& transport = *ctx.transport;
    applyTransition(transport, ctx.frames);

    const bool hasInput = ctx.numInputs > 0;
    AudioBus* in = hasInput ? &ctx.input(0) : nullptr;

    gain_.setTarget(dsp::dbToGain(paramValue(pGain_)));
    inputGain_.setTarget(dsp::dbToGain(paramValue(pInputGain_)));
    recordEnvelope_.setRampSeconds(std::max(0.001, static_cast<double>(paramValue(pFadeMs_)) * 0.001));

    const bool monitor = paramValue(pMonitor_) >= 0.5f;
    const float feedback = clampValue(paramValue(pFeedback_), 0.0f, 1.0f);
    const bool reverse = paramValue(pReverse_) >= 0.5f;
    const double speed = static_cast<double>(paramValue(pSpeed_));

    const bool playing = state_ == State::Playing || state_ == State::Overdubbing;
    const int channels = std::min(channels_, out.numChannels);

    for (int i = 0; i < ctx.frames; ++i) {
        const float outGain = gain_.next();
        const float inGain = inputGain_.next();
        const float recordAmount = recordEnvelope_.next();

        for (int c = 0; c < channels; ++c) {
            const float dry = (in && c < in->numChannels) ? in->chan(c)[i] * inGain : 0.0f;
            float wet = 0.0f;
            float* data = channelData(c);

            if (state_ == State::Recording) {
                if (writeHead_ < capacity_) data[writeHead_] = dry * recordAmount;
            } else if (playing && recordedFrames_ > 0) {
                wet = dsp::interpolateHermiteWrapped(data, recordedFrames_, readHead_);

                if (state_ == State::Overdubbing) {
                    // Write back at the integer position: overdubbing at a
                    // fractional read head would smear the layer.
                    auto index = static_cast<std::int64_t>(readHead_) % recordedFrames_;
                    if (index < 0) index += recordedFrames_;
                    data[index] = data[index] * feedback + dry * recordAmount;
                }
            }

            const float monitored = monitor ? dry : 0.0f;
            out.chan(c)[i] = (wet + monitored) * outGain;
        }

        if (state_ == State::Recording) {
            ++writeHead_;
            if (writeHead_ >= capacity_) {
                // Ran out of room: close the take rather than overwrite it.
                recordedFrames_ = capacity_;
                readHead_ = 0.0;
                recordEnvelope_.setTarget(0.0f);
                setState(State::Playing);
                uiLoopFrames_.store(recordedFrames_, std::memory_order_relaxed);
                overviewDirty_.store(true, std::memory_order_release);
            }
        } else if (playing && recordedFrames_ > 0) {
            readHead_ += reverse ? -speed : speed;
            const auto length = static_cast<double>(recordedFrames_);
            if (readHead_ >= length) readHead_ -= length;
            else if (readHead_ < 0.0) readHead_ += length;
        }
    }

    uiPosition_.store(state_ == State::Recording ? static_cast<double>(writeHead_) : readHead_,
                      std::memory_order_relaxed);
    if (state_ == State::Recording)
        uiLoopFrames_.store(std::max<std::int64_t>(writeHead_, 1), std::memory_order_relaxed);

    for (int c = 0; c < 2 && c < channels; ++c)
        meter_[c].store(follower_[c].process(out.chan(c), ctx.frames), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Message thread
// ---------------------------------------------------------------------------

void LooperNode::serviceFromMessageThread() {
    if (!overviewDirty_.exchange(false, std::memory_order_acquire)) return;

    overview_.assign(kOverviewBuckets, 0.0f);
    const std::int64_t frames = recordedFrames_;
    if (frames <= 0 || capacity_ <= 0) return;

    const double framesPerBucket = static_cast<double>(frames) / kOverviewBuckets;

    for (int b = 0; b < kOverviewBuckets; ++b) {
        const auto start = static_cast<std::int64_t>(b * framesPerBucket);
        auto end = static_cast<std::int64_t>((b + 1) * framesPerBucket);
        if (end <= start) end = start + 1;
        if (end > frames) end = frames;

        float peak = 0.0f;
        for (int c = 0; c < channels_; ++c) {
            const float* data = channelData(c);
            for (std::int64_t i = start; i < end; ++i) {
                const float a = data[i] < 0.0f ? -data[i] : data[i];
                if (a > peak) peak = a;
            }
        }
        overview_[static_cast<std::size_t>(b)] = peak;
    }
}

std::shared_ptr<SampleBuffer> LooperNode::captureTake() const {
    const std::int64_t frames = recordedFrames_;
    if (frames <= 0) return nullptr;

    // A snapshot, not a transaction: if the audio thread is mid-overdub the copy
    // catches the loop part way through a pass, which is fine for an export.
    auto take = std::make_shared<SampleBuffer>(channels_, frames, sampleRate_);
    for (int c = 0; c < channels_; ++c)
        std::memcpy(take->channelForWrite(c), channelData(c),
                    sizeof(float) * static_cast<std::size_t>(frames));

    take->displayName = name();
    take->computePeak();
    take->buildOverview();
    return take;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void LooperNode::saveExtraState(JsonValue& out) const {
    // Recorded audio is deliberately not written into the patch: takes are
    // performance material, and a patch file should stay small enough to email.
    // The length is kept so a reloaded patch shows the loop it expects.
    out.set("recordedFrames", static_cast<double>(recordedFrames_));
}

void LooperNode::loadExtraState(const JsonValue& in) {
    (void)in;
    recordedFrames_ = 0;
    uiLoopFrames_.store(0, std::memory_order_relaxed);
    setState(State::Empty);
}

} // namespace acm

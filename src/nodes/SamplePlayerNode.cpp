#include "SamplePlayerNode.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

// Below this the crossfade is not worth the arithmetic.
constexpr double kMinCrossfadeFrames = 8.0;

double fractionalPart(double v) noexcept {
    const double f = v - std::floor(v);
    return f < 0.0 ? f + 1.0 : f;
}

} // namespace

SamplePlayerNode::SamplePlayerNode() : Node("sample.player", NodeCategory::Source) {
    setName("Sample Player");
    addOutput("out", 2);

    addDbParam("gain", "Gain", -96.0f, 12.0f, 0.0f);
    pGain_ = indexOfParameter("gain");

    addFloatParam("pan", "Pan", -1.0f, 1.0f, 0.0f)
        .setDescription("Stereo placement. Hard left at -1, hard right at +1.");
    pPan_ = indexOfParameter("pan");

    addBoolParam("play", "Play", false)
        .setDescription("Runs the deck. In Gate mode the sample only sounds while this is on.");
    pPlay_ = indexOfParameter("play");

    addChoiceParam("mode", "Mode", { "loop", "one-shot", "gate" }, static_cast<int>(Mode::Loop));
    pMode_ = indexOfParameter("mode");

    addChoiceParam("sync", "Sync", { "free", "locked" }, static_cast<int>(Sync::Locked))
        .setDescription("Locked derives the read position from the transport, so the loop "
                        "cannot drift; Free runs on its own speed and pitch.");
    pSync_ = indexOfParameter("sync");

    addFloatParam("loopbeats", "Loop Length", 0.25f, 64.0f, 4.0f)
        .setUnit("beats")
        .setSkewForCentre(4.0f)
        .setDescription("How many beats the loop region is stretched to fill when locked.");
    pLoopBeats_ = indexOfParameter("loopbeats");

    addFloatParam("speed", "Speed", 0.125f, 4.0f, 1.0f)
        .setSkewForCentre(1.0f)
        .setUnit("x");
    pSpeed_ = indexOfParameter("speed");

    addFloatParam("pitch", "Pitch", -24.0f, 24.0f, 0.0f)
        .setUnit("st")
        .setDescription("Transposition in semitones. Resampling, so it changes length too.");
    pPitch_ = indexOfParameter("pitch");

    addFloatParam("start", "Start", 0.0f, 1.0f, 0.0f)
        .setDescription("Loop start as a fraction of the file.");
    pStart_ = indexOfParameter("start");

    addFloatParam("end", "End", 0.0f, 1.0f, 1.0f)
        .setDescription("Loop end as a fraction of the file.");
    pEnd_ = indexOfParameter("end");

    addChoiceParam("direction", "Direction", { "forward", "reverse", "ping-pong" },
                   static_cast<int>(Direction::Forward));
    pDirection_ = indexOfParameter("direction");

    addFloatParam("crossfade", "Loop X-fade", 0.0f, 250.0f, 4.0f)
        .setUnit("ms")
        .setDescription("Equal-power blend across the loop join, to hide a hard edit.");
    pCrossfade_ = indexOfParameter("crossfade");

    addChoiceParam("quantise", "Trigger Quantise", { "off", "beat", "bar" },
                   static_cast<int>(Quantise::Beat));
    pQuantise_ = indexOfParameter("quantise");

    addBoolParam("followtransport", "Follow Transport", true)
        .setDescription("Start and stop with the global transport as well as with Play.");
    pFollowTransport_ = indexOfParameter("followtransport");
}

// ---------------------------------------------------------------------------
// Sample management
// ---------------------------------------------------------------------------

bool SamplePlayerNode::loadFile(const std::string& utf8Path, std::string* error) {
    audiofile::LoadOptions options;
    options.buildOverview = true;

    std::string localError;
    auto loaded = audiofile::load(utf8Path, &localError, options);

    if (!loaded) {
        setErrorText(localError);
        if (error) *error = localError;
        return false;
    }

    samplePath_ = utf8Path;
    setErrorText({});
    if (name() == "Sample Player" || name().empty())
        setName(loaded->displayName.empty() ? pathStem(utf8Path) : loaded->displayName);

    // A file that carries its own loop points knows better than our defaults.
    if (loaded->hasEmbeddedLoop && loaded->frames() > 0) {
        const auto frames = static_cast<float>(loaded->frames());
        parameter(pStart_).setValue(static_cast<float>(loaded->loopStart) / frames);
        parameter(pEnd_).setValue(static_cast<float>(loaded->loopEnd) / frames);
    }

    setSample(std::move(loaded));
    return true;
}

void SamplePlayerNode::setSample(std::shared_ptr<SampleBuffer> sample) {
    uiSampleFrames_.store(sample ? sample->frames() : 0, std::memory_order_relaxed);
    sample_.publish(std::move(sample));
}

void SamplePlayerNode::clearSample() {
    samplePath_.clear();
    uiSampleFrames_.store(0, std::memory_order_relaxed);
    sample_.clear();
}

void SamplePlayerNode::stop() noexcept {
    stopRequested_.store(true, std::memory_order_release);
}

float SamplePlayerNode::playPositionNormalised() const noexcept {
    const auto frames = uiSampleFrames_.load(std::memory_order_relaxed);
    if (frames <= 0) return 0.0f;
    return clampValue(static_cast<float>(uiPosition_.load(std::memory_order_relaxed)
                                       / static_cast<double>(frames)), 0.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SamplePlayerNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    sample_.setClock(info.blockCounter);

    gain_.reset(info.sampleRate, 0.02);
    gain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pGain_)));
    pan_.reset(info.sampleRate, 0.02);
    pan_.setCurrentAndTarget(paramValue(pPan_));

    // 4 ms is short enough to feel instant and long enough to kill the click.
    envelope_.reset(info.sampleRate, 0.004);
    envelope_.setCurrentAndTarget(0.0f);

    for (auto& f : follower_) f.prepare(info.sampleRate);
    reset();
}

void SamplePlayerNode::reset() {
    position_ = 0.0;
    forward_ = true;
    sounding_ = false;
    pendingStart_ = false;
    envelope_.setCurrentAndTarget(0.0f);
    for (auto& f : follower_) f.reset();
    uiSounding_.store(false, std::memory_order_relaxed);
}

void SamplePlayerNode::serviceFromMessageThread() {
    sample_.collect();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void SamplePlayerNode::resolveLoopRegion(const SampleBuffer& s,
                                         double& startFrames, double& endFrames) const {
    const auto total = static_cast<double>(s.frames());
    double a = clampValue(static_cast<double>(paramValue(pStart_)), 0.0, 1.0) * total;
    double b = clampValue(static_cast<double>(paramValue(pEnd_)), 0.0, 1.0) * total;

    // Tolerate the handles being dragged past each other.
    if (b < a) std::swap(a, b);
    if (b - a < 4.0) b = std::min(total, a + 4.0);
    if (b - a < 4.0) { a = 0.0; b = total; }

    startFrames = a;
    endFrames = b;
}

float SamplePlayerNode::readSample(const SampleBuffer& s, int channel, double position) const {
    return dsp::interpolateHermite(s.channelWrapped(channel), s.frames(), position);
}

void SamplePlayerNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    out.clear();

    const SampleBuffer* s = sample_.get();
    if (s == nullptr || s->empty() || ctx.frames <= 0) {
        uiSounding_.store(false, std::memory_order_relaxed);
        for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
        return;
    }

    const TransportState& transport = *ctx.transport;

    const auto mode = static_cast<Mode>(static_cast<int>(paramValue(pMode_)));
    const auto sync = static_cast<Sync>(static_cast<int>(paramValue(pSync_)));
    const auto direction = static_cast<Direction>(static_cast<int>(paramValue(pDirection_)));
    const auto quantise = static_cast<Quantise>(static_cast<int>(paramValue(pQuantise_)));
    const bool followTransport = paramValue(pFollowTransport_) >= 0.5f;

    double loopStart = 0.0, loopEnd = 0.0;
    resolveLoopRegion(*s, loopStart, loopEnd);
    const double loopLength = loopEnd - loopStart;

    // -- transport / trigger logic -----------------------------------------

    const bool playFlag = paramValue(pPlay_) >= 0.5f;
    const bool playRising = playFlag && !lastPlayFlag_;
    const bool playFalling = !playFlag && lastPlayFlag_;
    lastPlayFlag_ = playFlag;

    bool wantsStart = playRising || triggerRequested_.exchange(false, std::memory_order_acquire);
    bool wantsStop = playFalling || stopRequested_.exchange(false, std::memory_order_acquire);

    if (followTransport) {
        // The global transport gates this deck too, so hitting stop silences the
        // whole patch without touching every node.
        if (!transport.playing && sounding_) wantsStop = true;
        if (transport.playing && playFlag && !sounding_ && !pendingStart_) wantsStart = true;
    }

    if (wantsStop) {
        sounding_ = false;
        pendingStart_ = false;
        envelope_.setTarget(0.0f);
    }

    if (wantsStart) {
        if (quantise == Quantise::Off || !transport.playing) {
            sounding_ = true;
            pendingStart_ = false;
            envelope_.setTarget(1.0f);
            position_ = (direction == Direction::Reverse) ? loopEnd - 1.0 : loopStart;
            forward_ = (direction != Direction::Reverse);
        } else {
            pendingStart_ = true;
        }
    }

    // A queued trigger fires the moment the transport crosses the next grid
    // line, which is what makes a quantised drop land where the performer meant.
    if (pendingStart_ && transport.playing) {
        const double gridBeats =
            (quantise == Quantise::Bar)
                ? (static_cast<double>(transport.timeSigNumerator) * 4.0
                   / static_cast<double>(transport.timeSigDenominator))
                : 1.0;

        const double blockBeats = (transport.samplesPerBeat > 0.0)
                                      ? static_cast<double>(ctx.frames) / transport.samplesPerBeat
                                      : 0.0;
        const double startBeat = transport.ppqPosition;
        const double endBeat = startBeat + blockBeats;

        const double nextGrid = std::floor(startBeat / gridBeats + 1.0e-9) * gridBeats + gridBeats;
        if (endBeat >= nextGrid || startBeat >= nextGrid) {
            sounding_ = true;
            pendingStart_ = false;
            envelope_.setTarget(1.0f);
            position_ = (direction == Direction::Reverse) ? loopEnd - 1.0 : loopStart;
            forward_ = (direction != Direction::Reverse);
        }
    }

    // -- read parameters that vary per block -------------------------------

    gain_.setTarget(dsp::dbToGain(paramValue(pGain_)));
    pan_.setTarget(paramValue(pPan_));

    const double rateRatio = s->sampleRate() / (ctx.sampleRate > 0.0 ? ctx.sampleRate : 48000.0);
    const double pitchRatio = std::pow(2.0, static_cast<double>(paramValue(pPitch_)) / 12.0);
    const double freeIncrement = static_cast<double>(paramValue(pSpeed_)) * pitchRatio * rateRatio;

    const double loopBeats = std::max(0.0625, static_cast<double>(paramValue(pLoopBeats_)));
    const double crossfadeFrames =
        std::max(0.0, static_cast<double>(paramValue(pCrossfade_)) * 0.001 * ctx.sampleRate);
    // A crossfade longer than half the loop would overlap itself.
    const bool useCrossfade = mode == Mode::Loop
                           && crossfadeFrames >= kMinCrossfadeFrames
                           && loopLength > crossfadeFrames * 2.5;

    const int channels = out.numChannels;
    const bool silent = !sounding_ && envelope_.current() <= 0.0f && !envelope_.smoothing();

    if (silent) {
        uiSounding_.store(false, std::memory_order_relaxed);
        for (auto& m : meter_) m.store(0.0f, std::memory_order_relaxed);
        // Keep the smoothers tracking so the next start does not jump.
        for (int i = 0; i < ctx.frames; ++i) { gain_.next(); pan_.next(); }
        return;
    }

    const double samplesPerBeat = transport.samplesPerBeat > 0.0 ? transport.samplesPerBeat : ctx.sampleRate;

    for (int i = 0; i < ctx.frames; ++i) {
        const float env = envelope_.next();
        const float g = gain_.next() * env;

        float panLeft = 1.0f, panRight = 1.0f;
        dsp::panGains(pan_.next(), panLeft, panRight);

        if (env <= 0.0f && !sounding_) {
            // Envelope has closed; nothing more to render this block.
            for (int c = 0; c < channels; ++c) out.chan(c)[i] = 0.0f;
            continue;
        }

        double readPosition = position_;
        double crossfadePosition = -1.0;
        float crossfadeMix = 0.0f;

        if (sync == Sync::Locked && loopLength > 0.0) {
            // Position is a pure function of musical time: no accumulation, so
            // no drift, and scrubbing the transport scrubs the loop.
            const double beats = transport.ppqPosition + static_cast<double>(i) / samplesPerBeat;
            double phase = fractionalPart(beats / loopBeats);

            if (direction == Direction::Reverse) {
                phase = 1.0 - phase;
            } else if (direction == Direction::PingPong) {
                // Fold the phase so the loop runs out and back within loopBeats.
                phase = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0;
            }

            readPosition = loopStart + phase * loopLength;
            position_ = readPosition;
        } else {
            // Free running.
            readPosition = position_;

            if (useCrossfade && forward_) {
                const double fadeStart = loopEnd - crossfadeFrames;
                if (readPosition >= fadeStart) {
                    const double t = (readPosition - fadeStart) / crossfadeFrames;
                    crossfadeMix = static_cast<float>(clampValue(t, 0.0, 1.0));
                    crossfadePosition = loopStart + (readPosition - fadeStart);
                }
            }

            const double increment = forward_ ? freeIncrement : -freeIncrement;
            position_ += increment;

            switch (mode) {
                case Mode::Loop:
                    if (direction == Direction::PingPong) {
                        if (position_ >= loopEnd) { position_ = loopEnd - (position_ - loopEnd); forward_ = false; }
                        else if (position_ <= loopStart) { position_ = loopStart + (loopStart - position_); forward_ = true; }
                    } else if (forward_) {
                        // Landing at loopStart + crossfade keeps the join seamless:
                        // that stretch was already heard as the fade-in.
                        if (position_ >= loopEnd)
                            position_ = loopStart + (useCrossfade ? crossfadeFrames : 0.0)
                                      + (position_ - loopEnd);
                    } else {
                        if (position_ <= loopStart) position_ = loopEnd - (loopStart - position_);
                    }
                    break;

                case Mode::OneShot:
                case Mode::Gate:
                    if ((forward_ && position_ >= loopEnd) || (!forward_ && position_ <= loopStart)) {
                        sounding_ = false;
                        envelope_.setTarget(0.0f);
                        position_ = clampValue(position_, loopStart, loopEnd);
                    }
                    break;
            }
        }

        for (int c = 0; c < channels; ++c) {
            float v = readSample(*s, c, readPosition);
            if (crossfadePosition >= 0.0) {
                const float other = readSample(*s, c, crossfadePosition);
                // Equal power across the join so the level does not dip.
                const float fadeOut = std::sqrt(1.0f - crossfadeMix);
                const float fadeIn = std::sqrt(crossfadeMix);
                v = v * fadeOut + other * fadeIn;
            }
            out.chan(c)[i] = v * g * (c == 0 ? panLeft : (c == 1 ? panRight : 1.0f));
        }
    }

    uiPosition_.store(position_, std::memory_order_relaxed);
    uiSounding_.store(sounding_, std::memory_order_relaxed);
    lastPpq_ = transport.ppqPosition;

    for (int c = 0; c < 2 && c < channels; ++c)
        meter_[c].store(follower_[c].process(out.chan(c), ctx.frames), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void SamplePlayerNode::saveExtraState(JsonValue& out) const {
    // Only the path is stored. Patches stay small and portable; a missing file
    // becomes a visible error on the node rather than a silent empty deck.
    if (!samplePath_.empty()) out.set("samplePath", samplePath_);
}

void SamplePlayerNode::loadExtraState(const JsonValue& in) {
    const std::string path = in.getString("samplePath");
    if (path.empty()) return;

    std::string error;
    if (!loadFile(path, &error))
        setErrorText("sample not loaded: " + error);
}

} // namespace acm

#include "BuildNode.h"

#include "../audio/AudioFile.h"
#include "../core/Graph.h"
#include "ColorNode.h"
#include "StemPlayerNode.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

// The divide choices the stem player offers, as indices into its parameter.
constexpr int kDivideChoiceCount = 6;   // 1, 2, 4, 8, 16, 32

} // namespace

BuildNode::BuildNode() : Node("build", NodeCategory::Effect) {
    addOutput("riser", 2);

    pEngage_ = indexOfParameter(addBoolParam(kEngageParam, "Engage", false).id());
    // Not captured by the metasurface: a build is a thing you do, not a place
    // you are, and having it come back on when a snapshot recalls would be a
    // nasty surprise mid-set.
    parameter(pEngage_).setAutomatable(true).setBlend(ParamBlend::Stepped);

    pBars_ = indexOfParameter(addIntParam("bars", "Build Length", 1, 32, 8).id());
    parameter(pBars_).setUnit("bars");

    pCurve_ = indexOfParameter(
        addChoiceParam("curve", "Curve", { "linear", "accelerating", "16ths" }, 1)
            .setBlend(ParamBlend::Stepped).id());

    pRelease_ = indexOfParameter(
        addChoiceParam("release", "Release", { "next bar", "next beat", "immediate" }, 0)
            .setBlend(ParamBlend::Stepped).id());

    pStartDivide_ = indexOfParameter(
        addChoiceParam("startDivide", "From", { "1", "2", "4", "8", "16", "32" }, 2)
            .setBlend(ParamBlend::Stepped).id());
    pEndDivide_ = indexOfParameter(
        addChoiceParam("endDivide", "To", { "1", "2", "4", "8", "16", "32" }, 4)
            .setBlend(ParamBlend::Stepped).id());

    pColorPush_ = indexOfParameter(addFloatParam("colorPush", "Colour Push", -1.0f, 1.0f, 1.0f).id());
    pRiserGain_ = indexOfParameter(addDbParam("riserGain", "Riser", -60.0f, 12.0f, -6.0f).id());
    pKillLow_ = indexOfParameter(addBoolParam("killLow", "Drop the Low End", true).id());

    // -- granular ----------------------------------------------------------
    // Grain size in milliseconds rather than samples: the musical question is
    // "how long is a grain", and the answer is in time, not in frames.
    pGrainSize_ = indexOfParameter(addFloatParam("grainSize", "Grain", 5.0f, 400.0f, 60.0f).id());
    parameter(pGrainSize_).setUnit("ms").setSkewForCentre(60.0f);

    pGrainDensity_ = indexOfParameter(
        addFloatParam("grainDensity", "Density", 1.0f, 120.0f, 18.0f).id());
    parameter(pGrainDensity_).setUnit("/s").setSkewForCentre(20.0f);

    pGrainPitch_ = indexOfParameter(addFloatParam("grainPitch", "Pitch", -24.0f, 24.0f, 0.0f).id());
    parameter(pGrainPitch_).setUnit("st");

    // How far through the snippet the cloud reads, and how far it scatters.
    pGrainSpread_ = indexOfParameter(addFloatParam("grainSpread", "Spread", 0.0f, 1.0f, 0.25f).id());
    pGrainGain_ = indexOfParameter(addDbParam("grainGain", "Grains", -60.0f, 12.0f, -60.0f).id());

    // What the build does to the cloud as it climbs: at 1 the grains get
    // shorter, denser and higher across the build, which is the sound of a
    // stutter tightening into a whine.
    pGrainRamp_ = indexOfParameter(addFloatParam("grainRamp", "Grain Ramp", 0.0f, 1.0f, 1.0f).id());
}

// ---------------------------------------------------------------------------
// Riser
// ---------------------------------------------------------------------------

bool BuildNode::loadRiser(const std::string& utf8Path, std::string* error) {
    std::string loadError;
    std::shared_ptr<SampleBuffer> buffer = audiofile::load(utf8Path, &loadError);
    if (!buffer) {
        if (error) *error = loadError;
        setErrorText(loadError);
        return false;
    }

    riser_.publish(std::move(buffer));
    riserPath_ = utf8Path;
    setErrorText({});
    return true;
}

void BuildNode::clearRiser() {
    riser_.clear();
    riserPath_.clear();
}

// ---------------------------------------------------------------------------
// Granular
// ---------------------------------------------------------------------------

void BuildNode::setSnippet(std::shared_ptr<SampleBuffer> snippet, std::string label) {
    if (!snippet || snippet->empty()) { clearSnippet(); return; }

    snippet_.publish(std::move(snippet));
    snippetLabel_ = std::move(label);
    setTrim(0.0f, 1.0f);
}

void BuildNode::clearSnippet() {
    snippet_.clear();
    snippetLabel_.clear();
    setTrim(0.0f, 1.0f);
}

void BuildNode::setTrim(float start, float end) noexcept {
    trimStart_ = clampValue(start, 0.0f, 1.0f);
    trimEnd_ = clampValue(end, 0.0f, 1.0f);
    // A zero-length or inverted trim would leave the grain scheduler with no
    // window to pick positions from, so the two are kept apart by a hair. Which
    // one moves depends on which end has room: pushing the end forward does
    // nothing when the start is already at the top.
    constexpr float kMinimumWindow = 0.01f;
    if (trimEnd_ - trimStart_ < kMinimumWindow) {
        if (trimStart_ + kMinimumWindow <= 1.0f) trimEnd_ = trimStart_ + kMinimumWindow;
        else { trimEnd_ = 1.0f; trimStart_ = 1.0f - kMinimumWindow; }
    }

    trimStartAtomic_.store(trimStart_, std::memory_order_relaxed);
    trimEndAtomic_.store(trimEnd_, std::memory_order_relaxed);
}

float BuildNode::nextRandom() noexcept {
    // xorshift: the grain cloud needs scatter, not statistical rigour, and this
    // costs nothing on the audio thread.
    grainRandom_ ^= grainRandom_ << 13;
    grainRandom_ ^= grainRandom_ >> 17;
    grainRandom_ ^= grainRandom_ << 5;
    return static_cast<float>(grainRandom_ & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

void BuildNode::renderGrains(AudioBus& out, int frames, float shaped, double sampleRate) noexcept {
    const SampleBuffer* source = snippet_.get();
    if (source == nullptr || source->empty()) return;

    const float gain = dsp::dbToGain(paramValue(pGrainGain_));
    if (gain <= 0.0f) return;

    const float ramp = paramValue(pGrainRamp_) * shaped;

    // The ramp shortens and tightens the cloud as the build climbs.
    const double sizeMs = static_cast<double>(paramValue(pGrainSize_)) * (1.0 - 0.75 * ramp);
    const double density = static_cast<double>(paramValue(pGrainDensity_)) * (1.0 + 5.0 * ramp);
    const double semitones = static_cast<double>(paramValue(pGrainPitch_)) + 12.0 * ramp;
    const float spread = paramValue(pGrainSpread_);

    const double rateRatio = source->sampleRate() / sampleRate;
    const double increment = rateRatio * std::pow(2.0, semitones / 12.0);
    const int grainLength = std::max(8, static_cast<int>(sizeMs * 0.001 * sampleRate));
    const double interval = sampleRate / std::max(1.0, density);

    const float trimStart = trimStartAtomic_.load(std::memory_order_relaxed);
    const float trimEnd = trimEndAtomic_.load(std::memory_order_relaxed);
    const double windowStart = static_cast<double>(source->frames()) * trimStart;
    const double windowLength = static_cast<double>(source->frames()) * (trimEnd - trimStart);
    if (windowLength <= 1.0) return;

    for (int i = 0; i < frames; ++i) {
        // -- schedule ------------------------------------------------------
        grainClock_ += 1.0;
        if (grainClock_ >= interval) {
            grainClock_ -= interval;

            for (Grain& grain : grains_) {
                if (grain.active) continue;

                grain.active = true;
                grain.length = grainLength;
                grain.remaining = grainLength;
                grain.increment = increment;
                grain.pan = nextRandom();

                // Scatter within the trimmed window. At zero spread every grain
                // starts at the same place, which is the stutter; at one they
                // are anywhere in it, which is the cloud.
                const double offset = static_cast<double>(nextRandom()) * spread * windowLength;
                grain.position = windowStart + offset;
                break;
            }
        }

        // -- render --------------------------------------------------------
        float left = 0.0f, right = 0.0f;

        for (Grain& grain : grains_) {
            if (!grain.active) continue;

            const std::int64_t index = static_cast<std::int64_t>(grain.position);
            if (index < 0 || index >= source->frames() - 1) { grain.active = false; continue; }

            // Raised cosine, so grains fade in and out rather than clicking -
            // at these lengths a hard edge is most of what you would hear.
            const float phase = 1.0f - static_cast<float>(grain.remaining)
                                     / static_cast<float>(std::max(1, grain.length));
            const float window = 0.5f - 0.5f * std::cos(phase * 2.0f * 3.14159265358979f);

            const float fraction = static_cast<float>(grain.position - static_cast<double>(index));
            const float* data = source->channel(0);
            const float value = (data[index] + (data[index + 1] - data[index]) * fraction) * window;

            left += value * (1.0f - grain.pan);
            right += value * grain.pan;

            grain.position += grain.increment;
            if (--grain.remaining <= 0) grain.active = false;
        }

        // Equal-ish power against the number of voices, so a dense cloud does
        // not simply get louder than a sparse one.
        const float normalise = gain * 0.35f;
        if (out.numChannels > 0) out.chan(0)[i] += left * normalise;
        if (out.numChannels > 1) out.chan(1)[i] += right * normalise;
    }
}

// ---------------------------------------------------------------------------
// Performance
// ---------------------------------------------------------------------------

void BuildNode::setEngaged(bool engage) noexcept {
    if (pEngage_ >= 0) parameter(pEngage_).setValue(engage ? 1.0f : 0.0f);
}

void BuildNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    riser_.setClock(info.blockCounter);
    snippet_.setClock(info.blockCounter);
    riserGain_.reset(info.sampleRate, 0.01);
    reset();
}

void BuildNode::reset() {
    running_ = false;
    riserPlaying_ = false;
    riserPosition_ = 0.0;
    releasePending_ = false;
    startPpq_ = 0.0;
    progress_.store(0.0f, std::memory_order_relaxed);
    publishedRunning_.store(false, std::memory_order_relaxed);
}

void BuildNode::process(ProcessContext& ctx) {
    ctx.clearOutputs();

    const int frames = ctx.frames;
    if (frames <= 0 || ctx.transport == nullptr) return;

    const TransportState& transport = *ctx.transport;
    const double beatsPerBar = static_cast<double>(std::max(1, transport.timeSigNumerator));
    const double bpm = transport.bpm > 0.0 ? transport.bpm : 120.0;
    const double beatsPerFrame = bpm / (60.0 * transport.sampleRate);

    const bool wanted = paramValue(pEngage_) > 0.5f;
    const double lengthBeats = static_cast<double>(std::max(1, static_cast<int>(
        std::lround(paramValue(pBars_))))) * beatsPerBar;

    const auto release = static_cast<Release>(
        static_cast<int>(std::lround(paramValue(pRelease_))));

    engaged_.store(wanted, std::memory_order_relaxed);

    // -- edges -------------------------------------------------------------
    if (wanted && !running_) {
        running_ = true;
        releasePending_ = false;
        startPpq_ = transport.ppqPosition;
        riserPosition_ = 0.0;
        riserPlaying_ = true;
    } else if (!wanted && running_ && !releasePending_) {
        // `!wanted && running_` stays true for every block between letting go
        // and the grid line arriving, so this has to be guarded on the pending
        // flag. Without it the release was announced once per block, and a drop
        // node listening fired on each announcement in turn.
        // Letting go slightly early should still land on the grid, so the
        // release is deferred to the next line unless it is set to immediate.
        //
        // The position is published here, when the release is *asked for*,
        // rather than below when it happens. Anything listening - the drop node
        // - may be scheduled ahead of this one, in which case it would see an
        // already-past event and be a whole block late. Announcing the grid
        // line in advance means it can wait exactly the right number of frames
        // whichever order the two run in.
        const double firstPrevious = transport.ppqPosition - beatsPerFrame;
        const double line = release == Release::NextBar
            ? (std::floor(firstPrevious / beatsPerBar) + 1.0) * beatsPerBar
            : std::floor(firstPrevious) + 1.0;

        if (release == Release::Immediate) {
            running_ = false;
            riserPlaying_ = false;
            releasePending_ = false;
            publishRelease(transport.ppqPosition);
        } else {
            releasePending_ = true;
            publishRelease(line);
        }
    }

    const SampleBuffer* riser = riser_.get();
    riserGain_.setTarget(running_ ? dsp::dbToGain(paramValue(pRiserGain_)) : 0.0f);

    AudioBus& out = ctx.output(0);

    for (int i = 0; i < frames; ++i) {
        const double ppq = transport.ppqPosition + static_cast<double>(i) * beatsPerFrame;
        const double previousPpq = ppq - beatsPerFrame;

        if (releasePending_) {
            const bool barEdge = std::floor(ppq / beatsPerBar) != std::floor(previousPpq / beatsPerBar);
            const bool beatEdge = std::floor(ppq) != std::floor(previousPpq);
            if ((release == Release::NextBar && barEdge)
                || (release == Release::NextBeat && beatEdge)) {
                running_ = false;
                riserPlaying_ = false;
                releasePending_ = false;
            }
        }

        if (running_) {
            const double elapsed = ppq - startPpq_;
            // Held past the end of the build, the progress stays pinned at the
            // top rather than wrapping - the drop happens when the switch is
            // let go, not when a timer runs out.
            const float p = static_cast<float>(clampValue(elapsed / lengthBeats, 0.0, 1.0));
            progress_.store(p, std::memory_order_relaxed);
        }

        const float gain = riserGain_.next();

        if (riserPlaying_ && riser != nullptr && !riser->empty() && gain > 0.0f) {
            const std::int64_t index = static_cast<std::int64_t>(riserPosition_);
            if (index >= 0 && index < riser->frames() - 1) {
                const float fraction = static_cast<float>(riserPosition_ - static_cast<double>(index));

                for (int c = 0; c < out.numChannels; ++c) {
                    const float* data = riser->channel(c < riser->channels() ? c : riser->channels() - 1);
                    out.chan(c)[i] = (data[index] + (data[index + 1] - data[index]) * fraction) * gain;
                }
            } else {
                riserPlaying_ = false;
            }

            riserPosition_ += riser->sampleRate() / transport.sampleRate;
        }
    }

    // The cloud only runs while the build does, and its shape follows the same
    // curve the loop divide climbs, so one gesture drives both.
    if (running_) renderGrains(out, frames, progress_.load(std::memory_order_relaxed),
                               transport.sampleRate);
    else for (Grain& grain : grains_) grain.active = false;

    publishedProgress_.store(progress_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    publishedRunning_.store(running_, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Driving the other nodes
// ---------------------------------------------------------------------------

void BuildNode::serviceFromMessageThread() {
    riser_.collect();
    snippet_.collect();
    driveTargets();
}

void BuildNode::driveTargets() {
    if (!graph_) return;

    const bool running = publishedRunning_.load(std::memory_order_acquire);
    const float progress = publishedProgress_.load(std::memory_order_relaxed);

    auto* stems = dynamic_cast<StemPlayerNode*>(graph_->node(stemPlayer_));
    auto* color = dynamic_cast<ColorNode*>(graph_->node(colorNode_));

    if (!running) {
        // Put everything back exactly once. Writing the rest position every
        // frame would stop the performer touching those controls by hand.
        if (drivingTargets_) {
            drivingTargets_ = false;

            if (stems) {
                if (const ParamIndex divide = stems->indexOfParameter(StemPlayerNode::kDivideParam);
                    divide >= 0)
                    stems->parameter(divide).setValue(0.0f);
                if (const ParamIndex repeat = stems->indexOfParameter(StemPlayerNode::kRepeatParam);
                    repeat >= 0)
                    stems->parameter(repeat).setValue(0.0f);
            }
            if (stems) stems->setChopMask(restoreChopMask_);
            if (color) color->setColor(restoreColor_);
        }
        return;
    }

    // First frame of a build: remember where the colour was so the drop returns
    // to the sound the track had, not to neutral.
    if (!drivingTargets_) {
        drivingTargets_ = true;
        restoreColor_ = color ? color->color() : 0.0f;
        restoreChopMask_ = stems ? stems->chopMask() : 0xFFFFFFFFu;
    }

    // -- loop divide -------------------------------------------------------
    const int startChoice = clampValue(static_cast<int>(std::lround(paramValue(pStartDivide_))),
                                       0, kDivideChoiceCount - 1);
    const int endChoice = clampValue(static_cast<int>(std::lround(paramValue(pEndDivide_))),
                                     0, kDivideChoiceCount - 1);

    const auto curve = static_cast<Curve>(static_cast<int>(std::lround(paramValue(pCurve_))));

    float shaped = progress;
    switch (curve) {
        case Curve::Accelerating:
            // Most of the movement in the last quarter, which is how a build
            // actually feels: nothing much, then everything at once.
            shaped = progress * progress * progress;
            break;
        case Curve::Stepped16:
            // Four hard steps rather than a ramp, for a stutter that arrives in
            // obvious jumps instead of sliding.
            shaped = std::floor(progress * 4.0f) / 3.0f;
            break;
        case Curve::Linear:
        default:
            break;
    }
    shaped = clampValue(shaped, 0.0f, 1.0f);

    if (stems) {
        const float choice = static_cast<float>(startChoice)
                           + (static_cast<float>(endChoice - startChoice)) * shaped;

        if (const ParamIndex divide = stems->indexOfParameter(StemPlayerNode::kDivideParam);
            divide >= 0)
            stems->parameter(divide).setValue(std::round(choice));

        // Repeat is on from the moment the switch goes down, not once the curve
        // has climbed far enough to divide the loop. A build-up whose chop only
        // arrives halfway through does not feel like it responded to the press,
        // and "From" now starts at a quarter so there is something to hear
        // immediately.
        if (const ParamIndex repeat = stems->indexOfParameter(StemPlayerNode::kRepeatParam);
            repeat >= 0)
            stems->parameter(repeat).setValue(1.0f);

        stems->setChopMask(chopMask_);

        // Dropping the low end is done by muting the stems the performer has
        // named as low - by convention the first two slots, drums and bass.
        // Muting rather than filtering because there is no filter here to use,
        // and because a stem player with the bass muted is unambiguous.
        if (paramValue(pKillLow_) > 0.5f && shaped > 0.6f) {
            for (int slot = 0; slot < 2; ++slot) {
                if (const ParamIndex mute = stems->indexOfParameter("mute" + std::to_string(slot + 1));
                    mute >= 0)
                    stems->parameter(mute).setValue(1.0f);
            }
        }
    }

    // -- colour ------------------------------------------------------------
    if (color) {
        const float push = paramValue(pColorPush_);
        color->setColor(restoreColor_ + (push - restoreColor_) * shaped);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void BuildNode::saveExtraState(JsonValue& out) const {
    out.set("riser", riserPath_);
    out.set("stemPlayer", static_cast<int>(stemPlayer_));
    out.set("colorNode", static_cast<int>(colorNode_));
    out.set("chopMask", static_cast<int>(chopMask_));
    out.set("trimStart", trimStart_);
    out.set("trimEnd", trimEnd_);
    out.set("snippetLabel", snippetLabel_);
}

void BuildNode::loadExtraState(const JsonValue& in) {
    stemPlayer_ = static_cast<NodeId>(in.getInt("stemPlayer", static_cast<int>(kInvalidNode)));
    colorNode_ = static_cast<NodeId>(in.getInt("colorNode", static_cast<int>(kInvalidNode)));

    chopMask_ = static_cast<std::uint32_t>(in.getInt("chopMask", 0x3));
    snippetLabel_ = in.getString("snippetLabel");
    setTrim(in.getFloat("trimStart", 0.0f), in.getFloat("trimEnd", 1.0f));

    if (const std::string path = in.getString("riser"); !path.empty())
        loadRiser(path, nullptr);
}

} // namespace acm

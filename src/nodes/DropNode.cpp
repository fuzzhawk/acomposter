#include "DropNode.h"

#include "../audio/AudioFile.h"
#include "../core/Graph.h"
#include "../core/FileIo.h"
#include "BuildNode.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

const char* kLayerIds[DropNode::kLayers] = { "a", "b", "c" };
// Named for what each layer is usually for rather than "1 2 3", so the node
// says what it is arranged to do.
const char* kLayerNames[DropNode::kLayers] = { "impact", "air", "body" };

} // namespace

DropNode::DropNode() : Node("drop", NodeCategory::Source) {
    addOutput("drop", 2);

    pGain_ = indexOfParameter(addDbParam("gain", "Gain", -60.0f, 12.0f, 0.0f).id());

    // A drop that rings through the first bar of the next section is a drop
    // that muddies it. The decay is a hard fade applied to every layer, so one
    // control shortens the whole impact.
    pDecay_ = indexOfParameter(addFloatParam("decay", "Decay", 0.05f, 8.0f, 8.0f).id());
    parameter(pDecay_).setUnit("s").setSkewForCentre(1.5f);

    for (int i = 0; i < kLayers; ++i) {
        Layer& layer = layers_[static_cast<std::size_t>(i)];
        const std::string id = kLayerIds[i];
        const std::string name = kLayerNames[i];

        layer.pGain = indexOfParameter(
            addDbParam(id + "Gain", name + " gain", -60.0f, 12.0f, 0.0f).id());
        layer.pPitch = indexOfParameter(
            addFloatParam(id + "Pitch", name + " pitch", -24.0f, 24.0f, 0.0f).id());
        parameter(layer.pPitch).setUnit("st");

        // Where in the file the layer starts, as a fraction. A crash sample
        // with half a second of nothing in front of it is common enough that
        // trimming it in the node beats re-editing the file.
        layer.pStart = indexOfParameter(
            addFloatParam(id + "Start", name + " start", 0.0f, 0.95f, 0.0f).id());

        layer.pMute = indexOfParameter(addBoolParam(id + "Mute", name + " mute", false).id());
    }
}

// ---------------------------------------------------------------------------
// Layers
// ---------------------------------------------------------------------------

bool DropNode::loadLayer(int layer, const std::string& utf8Path, std::string* error) {
    if (layer < 0 || layer >= kLayers) return false;

    std::string loadError;
    std::shared_ptr<SampleBuffer> buffer = audiofile::load(utf8Path, &loadError);
    if (!buffer) {
        if (error) *error = loadError;
        setErrorText(loadError);
        return false;
    }

    Layer& target = layers_[static_cast<std::size_t>(layer)];
    target.sample.publish(std::move(buffer));
    target.path = utf8Path;
    target.name = pathStem(utf8Path);
    setErrorText({});
    return true;
}

void DropNode::setLayer(int layer, std::shared_ptr<SampleBuffer> sample, std::string name) {
    if (layer < 0 || layer >= kLayers) return;
    if (!sample || sample->empty()) { clearLayer(layer); return; }

    Layer& target = layers_[static_cast<std::size_t>(layer)];
    target.sample.publish(std::move(sample));
    target.name = std::move(name);
    // No path: this buffer did not come from a file, so there is nothing for a
    // saved patch to reload it from, and claiming otherwise would produce a
    // patch that opens with a layer missing and no explanation.
    target.path.clear();
}

void DropNode::clearLayer(int layer) {
    if (layer < 0 || layer >= kLayers) return;
    Layer& target = layers_[static_cast<std::size_t>(layer)];
    target.sample.clear();
    target.path.clear();
    target.name.clear();
}

std::shared_ptr<SampleBuffer> DropNode::layerSample(int layer) const {
    if (layer < 0 || layer >= kLayers) return {};
    return layers_[static_cast<std::size_t>(layer)].sample.shared();
}

const std::string& DropNode::layerPath(int layer) const {
    static const std::string empty;
    if (layer < 0 || layer >= kLayers) return empty;
    return layers_[static_cast<std::size_t>(layer)].path;
}

const std::string& DropNode::layerName(int layer) const {
    static const std::string empty;
    if (layer < 0 || layer >= kLayers) return empty;
    return layers_[static_cast<std::size_t>(layer)].name;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

void DropNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    sampleRate_ = info.sampleRate;
    for (Layer& layer : layers_) layer.sample.setClock(info.blockCounter);
    reset();
}

void DropNode::reset() {
    for (Layer& layer : layers_) {
        layer.playing = false;
        layer.position = 0.0;
        layer.startOffset = 0;
    }
    progress_.store(-1.0f, std::memory_order_relaxed);
    releaseSeen_ = false;
    pending_ = false;
}

void DropNode::fire(int offset) noexcept {
    for (int i = 0; i < kLayers; ++i) {
        Layer& layer = layers_[static_cast<std::size_t>(i)];

        const SampleBuffer* sample = layer.sample.get();
        if (!sample || sample->empty()) { layer.playing = false; continue; }
        if (paramValue(layer.pMute) > 0.5f) { layer.playing = false; continue; }

        const double start = static_cast<double>(paramValue(layer.pStart))
                           * static_cast<double>(sample->frames());

        layer.position = start;
        layer.increment = sample->sampleRate() / sampleRate_
                        * std::pow(2.0, static_cast<double>(paramValue(layer.pPitch)) / 12.0);
        layer.startOffset = offset;
        layer.playing = true;
    }
}

void DropNode::process(ProcessContext& ctx) {
    ctx.clearOutputs();

    const int frames = ctx.frames;
    if (frames <= 0) return;

    const TransportState& transport = *ctx.transport;

    // -- triggers ----------------------------------------------------------
    // The manual one fires at the top of the block; there is nowhere better to
    // put a button press.
    const std::uint32_t manual = manualTrigger_.load(std::memory_order_acquire);
    if (manual != seenManual_) {
        seenManual_ = manual;
        fire(0);
    }

    if (graph_ != nullptr && buildNode_ != kInvalidNode) {
        if (const auto* build = dynamic_cast<const BuildNode*>(graph_->node(buildNode_))) {
            const BuildNode::ReleaseEvent event = build->lastRelease();

            // The first look only takes a reading. Without this a drop node
            // added to a patch that has already been played would fire the
            // moment it was created, on a release that happened minutes ago.
            if (!releaseSeen_) {
                seenRelease_ = event.count;
                releaseSeen_ = true;
            } else if (event.count != seenRelease_) {
                seenRelease_ = event.count;
                pending_ = true;
                pendingPpq_ = event.ppq;
            }
        }
    }

    if (pending_) {
        const double bpm = transport.bpm > 0.0 ? transport.bpm : 120.0;
        const double beatsPerFrame = bpm / (60.0 * transport.sampleRate);
        const double offset = beatsPerFrame > 0.0
            ? (pendingPpq_ - transport.ppqPosition) / beatsPerFrame
            : 0.0;

        // Ahead of this block: keep waiting. Behind it: the announcement only
        // reached here after the fact, so fire now and be a block late rather
        // than not at all. Inside it: the frame the build let go on.
        if (offset < static_cast<double>(frames)) {
            // Rounded, not truncated. The offset is a division of two positions
            // that are only approximately representable, so a release exactly
            // eight frames away arrives as 7.99999 and truncation puts the
            // impact a frame early - every time, in the same direction.
            const long rounded = std::lround(std::max(0.0, offset));
            fire(static_cast<int>(std::min<long>(rounded, frames - 1)));
            pending_ = false;
        }
    }

    // -- render ------------------------------------------------------------
    AudioBus& out = ctx.output(0);

    const float masterGain = dsp::dbToGain(paramValue(pGain_));
    const double decayFrames = std::max(1.0, static_cast<double>(paramValue(pDecay_)) * sampleRate_);

    float longest = -1.0f;

    for (int i = 0; i < kLayers; ++i) {
        Layer& layer = layers_[static_cast<std::size_t>(i)];
        if (!layer.playing) continue;

        const SampleBuffer* sample = layer.sample.get();
        if (!sample || sample->empty()) { layer.playing = false; continue; }

        const float gain = masterGain * dsp::dbToGain(paramValue(layer.pGain));
        const std::int64_t last = sample->frames() - 1;

        for (int f = 0; f < frames; ++f) {
            if (layer.startOffset > 0) { --layer.startOffset; continue; }

            const auto index = static_cast<std::int64_t>(layer.position);
            if (index < 0 || index >= last) { layer.playing = false; break; }

            // The decay counts from the start of the sample rather than from
            // the trigger, so moving a layer's start point does not also change
            // how long it rings.
            const double sounded = layer.position
                                 - static_cast<double>(paramValue(layer.pStart))
                                       * static_cast<double>(sample->frames());
            const float envelope = static_cast<float>(
                std::max(0.0, 1.0 - sounded / decayFrames));
            if (envelope <= 0.0f) { layer.playing = false; break; }

            const float fraction = static_cast<float>(layer.position - static_cast<double>(index));

            for (int c = 0; c < out.numChannels; ++c) {
                const float* data =
                    sample->channel(c < sample->channels() ? c : sample->channels() - 1);
                out.chan(c)[f] += (data[index] + (data[index + 1] - data[index]) * fraction)
                                * gain * envelope;
            }

            layer.position += layer.increment;
        }

        if (layer.playing && last > 0)
            longest = std::max(longest, static_cast<float>(layer.position
                                                           / static_cast<double>(last)));
    }

    progress_.store(longest, std::memory_order_relaxed);
}

void DropNode::serviceFromMessageThread() {
    for (Layer& layer : layers_) layer.sample.collect();
}

void DropNode::saveExtraState(JsonValue& out) const {
    out.set("buildNode", static_cast<int>(buildNode_));
    for (int i = 0; i < kLayers; ++i)
        out.set(std::string("layer") + kLayerIds[i],
                layers_[static_cast<std::size_t>(i)].path);
}

void DropNode::loadExtraState(const JsonValue& in) {
    buildNode_ = static_cast<NodeId>(in.getInt("buildNode", static_cast<int>(kInvalidNode)));

    for (int i = 0; i < kLayers; ++i) {
        const std::string path = in.getString(std::string("layer") + kLayerIds[i]);
        if (!path.empty()) loadLayer(i, path, nullptr);
    }
}

} // namespace acm

#include "VstNode.h"

#include "../core/Base64.h"
#include "../core/FileIo.h"
#include "../dsp/Dsp.h"
#include "../nodes/NodeFactory.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace acm::vst2 {

const char* VstNode::kTypeName = "vst2.plugin";
PluginManager* VstNode::manager_ = nullptr;

namespace {

// Stereo in and out is the layout the patcher works in; a plugin with a
// different count is adapted around rather than exposed raw, so a 4-in
// mastering plugin does not sprout four ports on the canvas.
constexpr int kBusChannels = 2;

} // namespace

VstNode::VstNode(const PluginDescription& description)
    : Node(kTypeName, description.isSynth ? NodeCategory::Source : NodeCategory::Effect),
      description_(description) {
    setName(description.name.empty() ? "VST Plugin" : description.name);

    // A synth still gets an input: plenty of VST2 instruments accept audio, and
    // an unconnected port costs nothing.
    addInput("in", kBusChannels);
    addOutput("out", kBusChannels);

    addFloatParam("drywet", "Dry/Wet", 0.0f, 1.0f, 1.0f)
        .setDescription("Blends the plugin's output against its input. Fully wet by default.");
    pDryWet_ = indexOfParameter("drywet");

    addDbParam("outgain", "Output", -96.0f, 12.0f, 0.0f);
    pOutputGain_ = indexOfParameter("outgain");

    // Plugin parameters are mirrored one-for-one, in the plugin's own order, so
    // patches keep working when a plugin is updated as long as its parameter
    // ordering is stable (which the VST2 ABI effectively requires).
    firstPluginParam_ = numParameters();

    for (int i = 0; i < description.numParameters; ++i) {
        // Real names arrive once the plugin is instantiated; the placeholder
        // keeps the ids stable regardless of whether the load succeeds.
        auto& parameter = addFloatParam("p" + std::to_string(i),
                                        "Param " + std::to_string(i + 1), 0.0f, 1.0f, 0.0f);
        parameter.setDescription("Plugin parameter " + std::to_string(i + 1));
    }

    lastPushed_.assign(static_cast<std::size_t>(std::max(0, description.numParameters)), -1.0f);
}

VstNode::~VstNode() {
    if (plugin_) {
        plugin_->closeEditor();
        plugin_->setActive(false);
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

bool VstNode::loadPlugin(const PluginManager& manager, double sampleRate, int blockSize,
                         bool forceBridge, std::string* error) {
    std::string localError;
    plugin_ = manager.instantiate(description_, sampleRate, blockSize, forceBridge, &localError);

    if (!plugin_) {
        setErrorText(localError.empty() ? "the plugin could not be loaded" : localError);
        if (error) *error = errorText();
        return false;
    }

    bridged_ = PluginManager::requiresBridge(description_.architecture) || forceBridge;

    // The instantiated plugin is the authority on its own description; the
    // scan-cache entry can be stale.
    const PluginDescription& actual = plugin_->description();
    description_.numParameters = actual.numParameters;
    description_.numPrograms = actual.numPrograms;
    description_.numInputs = actual.numInputs;
    description_.numOutputs = actual.numOutputs;
    description_.hasEditor = actual.hasEditor;
    description_.usesChunks = actual.usesChunks;
    if (!actual.name.empty()) description_.name = actual.name;

    setErrorText({});
    mirrorParameters();

    if (!pendingState_.empty()) {
        plugin_->restoreState(pendingState_);
        pendingState_.clear();
        pullParametersFromPlugin();
    }

    plugin_->setActive(true);
    active_ = true;
    return true;
}

void VstNode::mirrorParameters() {
    if (!plugin_) return;

    const int count = std::min(plugin_->parameterCount(),
                               numParameters() - firstPluginParam_);

    for (int i = 0; i < count; ++i) {
        Parameter& parameter = this->parameter(firstPluginParam_ + i);
        // Rename in place: the id stays "pN" so saved patches keep matching,
        // while the label the performer sees is the plugin's own.
        const std::string name = plugin_->parameterName(i);
        const std::string label = plugin_->parameterLabel(i);
        parameter.setDescription(name + (label.empty() ? "" : " (" + label + ")"));
        parameter.setValue(plugin_->parameterValue(i));
    }

    lastPushed_.assign(static_cast<std::size_t>(std::max(0, count)), -1.0f);
}

void VstNode::pullParametersFromPlugin() {
    if (!plugin_) return;

    const int count = std::min(plugin_->parameterCount(), numParameters() - firstPluginParam_);
    for (int i = 0; i < count; ++i) {
        const float value = plugin_->parameterValue(i);
        parameter(firstPluginParam_ + i).setValue(value);
        if (i < static_cast<int>(lastPushed_.size()))
            lastPushed_[static_cast<std::size_t>(i)] = value;
    }
}

void VstNode::pushChangedParameters() {
    if (!plugin_) return;

    const int count = std::min({ plugin_->parameterCount(),
                                 numParameters() - firstPluginParam_,
                                 static_cast<int>(lastPushed_.size()) });

    for (int i = 0; i < count; ++i) {
        const float value = paramValue(firstPluginParam_ + i);
        // Only genuine movement crosses the boundary. With a bridged plugin
        // every push costs bytes in the shared block, and with a native one a
        // redundant setParameter can still make a plugin recompute coefficients.
        if (value != lastPushed_[static_cast<std::size_t>(i)]) {
            plugin_->setParameterValue(i, value);
            lastPushed_[static_cast<std::size_t>(i)] = value;
        }
    }
}

bool VstNode::reloadPlugin() {
    if (!manager_) return false;

    const bool wasEditorOpen = editorOpen();
    if (plugin_) {
        pendingState_ = plugin_->saveState();
        plugin_->closeEditor();
        plugin_->setActive(false);
        plugin_.reset();
    }
    active_ = false;

    std::string error;
    if (!loadPlugin(*manager_, sampleRate(), maxBlockSize(), pendingForceBridge_, &error))
        return false;

    if (wasEditorOpen && plugin_) plugin_->openEditor();
    return true;
}

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------

bool VstNode::editorOpen() const {
    return plugin_ && plugin_->editorOpen();
}

void VstNode::toggleEditor() {
    if (!plugin_ || !plugin_->hasEditor()) return;
    if (plugin_->editorOpen()) plugin_->closeEditor();
    else plugin_->openEditor();
}

void VstNode::closeEditor() {
    if (plugin_) plugin_->closeEditor();
}

// ---------------------------------------------------------------------------
// Programs and MIDI
// ---------------------------------------------------------------------------

int VstNode::programCount() const { return plugin_ ? plugin_->programCount() : 0; }
int VstNode::currentProgram() const { return plugin_ ? plugin_->currentProgram() : 0; }

void VstNode::setCurrentProgram(int index) {
    if (!plugin_) return;
    plugin_->setCurrentProgram(index);
    pullParametersFromPlugin();
}

std::string VstNode::programName(int index) const {
    return plugin_ ? plugin_->programName(index) : std::string();
}

void VstNode::sendMidi(unsigned char status, unsigned char data1, unsigned char data2) {
    if (plugin_) plugin_->sendMidi(MidiMessage{ status, data1, data2, 0 });
}

void VstNode::allNotesOff() {
    if (!plugin_) return;
    // Both the controller message and an explicit sweep: not every plugin
    // honours CC 123, and a stuck note during a set is unforgivable.
    for (unsigned char channel = 0; channel < 16; ++channel) {
        plugin_->sendMidi(MidiMessage{ static_cast<unsigned char>(0xB0 | channel), 123, 0, 0 });
        plugin_->sendMidi(MidiMessage{ static_cast<unsigned char>(0xB0 | channel), 120, 0, 0 });
    }
}

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

void VstNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);

    dryWet_.reset(info.sampleRate, 0.02);
    dryWet_.setCurrentAndTarget(paramValue(pDryWet_));
    outputGain_.reset(info.sampleRate, 0.02);
    outputGain_.setCurrentAndTarget(dsp::dbToGain(paramValue(pOutputGain_)));

    const int pluginInputs = std::max(0, description_.numInputs);
    const int pluginOutputs = std::max(0, description_.numOutputs);

    inputPointers_.assign(static_cast<std::size_t>(pluginInputs), nullptr);
    outputPointers_.assign(static_cast<std::size_t>(pluginOutputs), nullptr);

    // The plugin writes into our own storage rather than straight into the
    // graph's output buffer, because the dry/wet blend needs the input intact.
    const int wetChannels = std::max({ pluginOutputs, kBusChannels, 1 });
    wetStorage_.assign(static_cast<std::size_t>(wetChannels)
                           * static_cast<std::size_t>(info.maxBlockSize), 0.0f);
    wetPointers_.assign(static_cast<std::size_t>(wetChannels), nullptr);
    for (int c = 0; c < wetChannels; ++c)
        wetPointers_[static_cast<std::size_t>(c)] =
            wetStorage_.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(info.maxBlockSize);

    if (plugin_) {
        plugin_->setSampleRateAndBlockSize(info.sampleRate, info.maxBlockSize);
        plugin_->setActive(true);
        active_ = true;
    }
}

void VstNode::reset() {
    if (plugin_) {
        plugin_->setActive(false);
        plugin_->setActive(true);
    }
    std::fill(wetStorage_.begin(), wetStorage_.end(), 0.0f);
}

int VstNode::latencyFrames() const {
    return plugin_ ? plugin_->latencyFrames() : 0;
}

void VstNode::process(ProcessContext& ctx) {
    if (ctx.numOutputs == 0) return;

    AudioBus& out = ctx.output(0);
    AudioBus* in = ctx.numInputs > 0 ? &ctx.input(0) : nullptr;

    dryWet_.setTarget(clampValue(paramValue(pDryWet_), 0.0f, 1.0f));
    outputGain_.setTarget(dsp::dbToGain(paramValue(pOutputGain_)));

    // A plugin that failed or whose bridge died passes audio through untouched.
    // Silence would be the wrong failure mode in the middle of a set.
    if (!plugin_ || !plugin_->valid()) {
        for (int c = 0; c < out.numChannels; ++c) {
            const float* source = in ? in->chan(c % in->numChannels) : nullptr;
            float* destination = out.chan(c);
            for (int i = 0; i < ctx.frames; ++i) {
                const float gain = (c == 0) ? outputGain_.next() : outputGain_.current();
                destination[i] = source ? source[i] * gain : 0.0f;
            }
        }
        return;
    }

    pushChangedParameters();

    const int pluginInputs = static_cast<int>(inputPointers_.size());
    const int pluginOutputs = static_cast<int>(outputPointers_.size());

    for (int c = 0; c < pluginInputs; ++c) {
        inputPointers_[static_cast<std::size_t>(c)] =
            in ? in->chan(c % in->numChannels) : nullptr;
    }
    for (int c = 0; c < pluginOutputs && c < static_cast<int>(wetPointers_.size()); ++c)
        outputPointers_[static_cast<std::size_t>(c)] = wetPointers_[static_cast<std::size_t>(c)];

    HostTimeInfo time;
    if (ctx.transport) {
        const TransportState& transport = *ctx.transport;
        time.samplePosition = static_cast<double>(transport.samplePosition);
        time.sampleRate = transport.sampleRate;
        time.ppqPosition = transport.ppqPosition;
        time.tempo = transport.bpm;
        time.timeSigNumerator = transport.timeSigNumerator;
        time.timeSigDenominator = transport.timeSigDenominator;
        time.playing = transport.playing;
        time.recording = transport.recording;
        time.cycleActive = transport.loopEnabled;
        time.cycleStart = transport.loopStartPpq;
        time.cycleEnd = transport.loopEndPpq;

        const double beatsPerBar = static_cast<double>(transport.timeSigNumerator) * 4.0
                                 / static_cast<double>(transport.timeSigDenominator);
        if (beatsPerBar > 0.0) {
            const double bars = std::floor(transport.ppqPosition / beatsPerBar);
            time.barStartPosition = bars * beatsPerBar;
        }
    }

    plugin_->process(inputPointers_.empty() ? nullptr : inputPointers_.data(), pluginInputs,
                     outputPointers_.empty() ? nullptr : outputPointers_.data(), pluginOutputs,
                     ctx.frames, time);

    // Blend the plugin's output back against the dry signal and apply trim.
    for (int i = 0; i < ctx.frames; ++i) {
        const float wetAmount = dryWet_.next();
        const float dryAmount = 1.0f - wetAmount;
        const float gain = outputGain_.next();

        for (int c = 0; c < out.numChannels; ++c) {
            const float dry = in ? in->chan(c % in->numChannels)[i] : 0.0f;
            const float wet = (pluginOutputs > 0)
                                  ? wetPointers_[static_cast<std::size_t>(c % pluginOutputs)][i]
                                  : 0.0f;
            out.chan(c)[i] = (wet * wetAmount + dry * dryAmount) * gain;
        }
    }
}

void VstNode::serviceFromMessageThread() {
    if (!plugin_) return;

    plugin_->idle();

    // A plugin whose own editor moved a control, or which loaded a preset,
    // reports it here; re-read so the canvas and the metasurface agree with what
    // the plugin actually has.
    if (plugin_->consumeParameterRefreshFlag())
        pullParametersFromPlugin();

    if (!plugin_->valid() && errorText().empty())
        setErrorText(plugin_->errorText().empty() ? "the plugin stopped responding"
                                                  : plugin_->errorText());
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void VstNode::saveExtraState(JsonValue& out) const {
    out.set("pluginIdentifier", description_.identifier());
    out.set("pluginPath", description_.path);
    out.set("pluginName", description_.name);
    out.set("uniqueId", static_cast<double>(description_.uniqueId));
    out.set("architecture", static_cast<int>(description_.architecture));
    out.set("numParameters", description_.numParameters);
    out.set("numInputs", description_.numInputs);
    out.set("numOutputs", description_.numOutputs);
    out.set("isSynth", description_.isSynth);
    out.set("hasEditor", description_.hasEditor);
    out.set("usesChunks", description_.usesChunks);
    out.set("forceBridge", pendingForceBridge_);

    // The plugin's own state, base64-encoded so the patch stays a text file.
    if (plugin_ && plugin_->valid()) {
        const std::vector<std::uint8_t> state = plugin_->saveState();
        if (!state.empty()) out.set("pluginState", base64Encode(state));
    } else if (!pendingState_.empty()) {
        // The plugin never loaded; carry the state we were given straight back
        // out so a save/load cycle on a machine without it is not destructive.
        out.set("pluginState", base64Encode(pendingState_));
    }
}

void VstNode::loadExtraState(const JsonValue& in) {
    pendingIdentifier_ = in.getString("pluginIdentifier");
    pendingPath_ = in.getString("pluginPath");
    pendingForceBridge_ = in.getBool("forceBridge", false);

    const std::string encoded = in.getString("pluginState");
    if (!encoded.empty()) pendingState_ = base64Decode(encoded);
}

std::unique_ptr<Node> VstNode::createFromPatchState(const JsonValue& state,
                                                    double sampleRate, int blockSize,
                                                    std::string* error) {
    if (!manager_) {
        if (error) *error = "the plugin manager is not available";
        return nullptr;
    }

    const std::string identifier = state.getString("pluginIdentifier");
    const std::string path = state.getString("pluginPath");

    const PluginDescription* found = manager_->findForPatch(identifier, path);

    // The patch records enough to rebuild the node's shape even when the plugin
    // itself is missing, so the canvas still shows the box, its connections
    // survive, and the metasurface keeps its parameter addresses.
    PluginDescription description;
    if (found) {
        description = *found;
    } else {
        description.path = path;
        description.name = state.getString("pluginName", "Missing Plugin");
        description.uniqueId = static_cast<std::int32_t>(state.getInt64("uniqueId", 0));
        description.numParameters = state.getInt("numParameters", 0);
        description.numInputs = state.getInt("numInputs", 2);
        description.numOutputs = state.getInt("numOutputs", 2);
        description.isSynth = state.getBool("isSynth", false);
        description.hasEditor = state.getBool("hasEditor", false);
        description.usesChunks = state.getBool("usesChunks", false);

        const int architecture = state.getInt("architecture", 0);
        description.architecture = (architecture == 1) ? Architecture::X86
                                 : (architecture == 2) ? Architecture::X64
                                                       : Architecture::Unknown;
    }

    auto node = std::make_unique<VstNode>(description);
    node->loadExtraState(state);

    if (!found) {
        node->setErrorText("plugin not found: " + (path.empty() ? identifier : pathLeaf(path)));
        if (error) *error = node->errorText();
        return node;   // deliberately still returned, as a placeholder
    }

    std::string loadError;
    node->loadPlugin(*manager_, sampleRate, blockSize, node->pendingForceBridge_, &loadError);
    if (error) *error = loadError;

    return node;
}

// ---------------------------------------------------------------------------

void registerVstNodeLoader(double sampleRate, int blockSize) {
    NodeFactory::instance().setExternalLoader(
        [sampleRate, blockSize](const std::string& typeName, const JsonValue& state)
            -> std::unique_ptr<Node> {
            if (typeName != VstNode::kTypeName) return nullptr;

            std::string error;
            return VstNode::createFromPatchState(state, sampleRate, blockSize, &error);
        });
}

} // namespace acm::vst2

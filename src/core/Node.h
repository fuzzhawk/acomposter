// Base class for everything that lives on the patcher canvas.
//
// A node owns its ports and parameters, declares them once at construction, and
// implements process(). Node subclasses know nothing about the UI: the patcher
// draws them generically from their port and parameter descriptions, and only
// reaches for a specific type when it wants a richer inline editor (a waveform
// strip for the sample player, an X/Y pad for the crossfader).
#pragma once

#include "AudioBuffer.h"
#include "Json.h"
#include "Parameter.h"
#include "Transport.h"
#include "Types.h"

#include <string>
#include <string_view>
#include <vector>

namespace acm {

// Drives the accent colour and the palette grouping in the UI.
enum class NodeCategory : std::uint8_t {
    Source,     // sample player, oscillators, live input
    Effect,     // VST, filters
    Mixing,     // mixer, crossfader, gain
    Routing,    // splitters, sends
    Output,     // audio out
    Analysis,   // meters, scopes
};

const char* toString(NodeCategory c) noexcept;

struct PrepareInfo {
    double sampleRate = 48000.0;
    int maxBlockSize = 512;
};

// Everything a node needs for one render block. Buses point into graph-owned
// storage and are only valid for the duration of the call.
struct ProcessContext {
    int frames = 0;
    double sampleRate = 48000.0;
    const TransportState* transport = nullptr;
    std::uint64_t streamFrame = 0;  // frames since the device opened

    AudioBus* inputs = nullptr;
    int numInputs = 0;
    AudioBus* outputs = nullptr;
    int numOutputs = 0;

    // The hardware edges. The engine fills deviceInput before rendering and
    // reads deviceOutput afterwards; the AudioIn and AudioOut nodes are the only
    // things that touch them, but routing them through the context keeps the
    // graph free of any special-case knowledge about which node is which.
    const AudioBuffer* deviceInput = nullptr;
    AudioBuffer* deviceOutput = nullptr;

    AudioBus& input(int i) noexcept { return inputs[i]; }
    AudioBus& output(int i) noexcept { return outputs[i]; }

    void clearOutputs() noexcept {
        for (int i = 0; i < numOutputs; ++i) outputs[i].clear();
    }
};

class Node {
public:
    explicit Node(std::string typeName, NodeCategory category = NodeCategory::Effect);
    virtual ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // -- identity ----------------------------------------------------------
    NodeId id() const noexcept { return id_; }
    void setId(NodeId id) noexcept { id_ = id; }

    // Stable type key used by the factory and the patch format.
    const std::string& typeName() const noexcept { return typeName_; }

    // User-editable label shown on the canvas.
    const std::string& name() const noexcept { return name_; }
    void setName(std::string n) { name_ = std::move(n); }

    NodeCategory category() const noexcept { return category_; }

    // -- ports -------------------------------------------------------------
    int numInputs() const noexcept { return static_cast<int>(inputPorts_.size()); }
    int numOutputs() const noexcept { return static_cast<int>(outputPorts_.size()); }
    const PortDescriptor& inputPort(int i) const { return inputPorts_[static_cast<std::size_t>(i)]; }
    const PortDescriptor& outputPort(int i) const { return outputPorts_[static_cast<std::size_t>(i)]; }

    // -- parameters --------------------------------------------------------
    int numParameters() const noexcept { return static_cast<int>(params_.size()); }
    Parameter& parameter(int i) { return *params_[static_cast<std::size_t>(i)]; }
    const Parameter& parameter(int i) const { return *params_[static_cast<std::size_t>(i)]; }
    Parameter* findParameter(std::string_view id);
    const Parameter* findParameter(std::string_view id) const;
    ParamIndex indexOfParameter(std::string_view id) const;

    // Convenience for process(): reads the current value of parameter `i`.
    float paramValue(int i) const noexcept { return params_[static_cast<std::size_t>(i)]->value(); }

    // -- bypass ------------------------------------------------------------
    bool bypassed() const noexcept { return bypassed_.load(std::memory_order_relaxed); }
    void setBypassed(bool b) noexcept { bypassed_.store(b, std::memory_order_relaxed); }

    // -- lifecycle ---------------------------------------------------------
    // Called on the message thread while the audio callback is suspended.
    virtual void prepare(const PrepareInfo& info);
    // Flush any internal state (delay lines, envelopes) without reallocating.
    virtual void reset() {}
    // The render call. Must not allocate, lock, or block.
    virtual void process(ProcessContext& ctx) = 0;

    // Latency this node introduces, in frames. Reported for display; the graph
    // does not currently compensate automatically.
    virtual int latencyFrames() const { return 0; }

    // Called once per UI frame on the message thread. Somewhere for nodes to do
    // deferred work: retire old buffers, pump a plugin's message loop.
    virtual void serviceFromMessageThread() {}

    // -- persistence -------------------------------------------------------
    // Parameters are saved by the patch writer automatically. Override these for
    // anything else: sample file paths, VST state chunks, looper contents.
    virtual void saveExtraState(JsonValue& out) const { (void)out; }
    virtual void loadExtraState(const JsonValue& in) { (void)in; }

    // -- canvas placement (owned by the editor, persisted with the patch) ---
    float canvasX = 0.0f;
    float canvasY = 0.0f;
    float canvasWidth = 0.0f;   // 0 = use the type's natural width
    bool collapsed = false;

    // Free-text note the performer can pin to a node.
    std::string comment;

    // -- diagnostics -------------------------------------------------------
    // Rolling share of the block budget this node consumed, 0..1.
    float cpuLoad() const noexcept { return cpuLoad_.load(std::memory_order_relaxed); }
    void reportCpuLoad(float load) noexcept { cpuLoad_.store(load, std::memory_order_relaxed); }

    // Set by nodes that have failed in a way worth surfacing (missing sample
    // file, plugin that would not load). Empty means healthy.
    const std::string& errorText() const noexcept { return errorText_; }
    void setErrorText(std::string t) { errorText_ = std::move(t); }

protected:
    // Declaration helpers, called from subclass constructors.
    void addInput(std::string name, int channels = 2, bool sidechain = false);
    void addOutput(std::string name, int channels = 2);

    Parameter& addParameter(std::string id, std::string name, ParamKind kind,
                            float minValue, float maxValue, float defaultValue);
    Parameter& addFloatParam(std::string id, std::string name, float lo, float hi, float def);
    Parameter& addDbParam(std::string id, std::string name, float lo, float hi, float def);
    Parameter& addBoolParam(std::string id, std::string name, bool def);
    Parameter& addChoiceParam(std::string id, std::string name, std::vector<std::string> choices, int def);
    Parameter& addIntParam(std::string id, std::string name, int lo, int hi, int def);

    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlockSize_; }

private:
    NodeId id_ = kInvalidNode;
    std::string typeName_;
    std::string name_;
    NodeCategory category_ = NodeCategory::Effect;

    std::vector<PortDescriptor> inputPorts_;
    std::vector<PortDescriptor> outputPorts_;
    std::vector<ParameterPtr> params_;

    std::atomic<bool> bypassed_{ false };
    std::atomic<float> cpuLoad_{ 0.0f };
    std::string errorText_;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
};

} // namespace acm

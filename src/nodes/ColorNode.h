// The colour engine: one knob that moves a whole effect chain between two
// captured states, through an untouched middle.
//
// The axis runs red to blue. Red is the tight, dry, low-heavy end; blue is the
// washed-out, high-passed, long-tailed end. Dead centre is *neutral*, and
// neutral means every targeted parameter sits at the value it had when the
// preset was made - so a colour of zero is audibly the chain doing nothing,
// which is what makes the knob safe to leave in the middle during a set.
//
// It does not contain the plugins. It drives them. The plugins are ordinary
// nodes in the patch, wired in whatever order suits, and this node holds a list
// of (node, parameter) targets with three values each: red, neutral and blue.
// That matters for a reason worth stating plainly.
//
// The obvious design - hard-code the parameter indices for FabFilter Volcano,
// Pro-Q, FXpansion Bloom and 2CAudio Aether - cannot be made to work. None of
// those plugins publish a stable index map, and several of them do not have one:
// Pro-Q exposes bands as they are created, Volcano's modulation slots move with
// the routing, Bloom's mod matrix is user-built. An index list would be wrong
// for a different band count and would break silently on the next plugin update,
// writing the reverb's decay into its mix control mid-set.
//
// So targets are discovered at runtime from whatever the plugin reports, and the
// two ends are *captured* from the plugin's own interface rather than typed in:
// dial the sound you want, press "set red", dial the other, press "set blue".
// The presets that ship name their targets by matching the parameter names the
// plugin reports, which is stable across versions in a way indices are not, and
// they fall back to doing nothing rather than to guessing.
#pragma once

#include "../core/Node.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace acm {

class Graph;

// One driven parameter and where it sits at each end of the axis. Values are
// normalised, because interpolating in normalised space is the only way a blend
// between 20 Hz and 20 kHz is a filter sweep rather than a jump.
struct ColorTarget {
    ParamAddress address;

    float redValue = 0.5f;
    float neutralValue = 0.5f;
    float blueValue = 0.5f;

    // How much of the axis this target responds to. A parameter set to 0.25
    // moves a quarter as far as the knob does, which is how one chain can have
    // a filter that sweeps the whole way and a mix control that barely stirs.
    float depth = 1.0f;

    bool enabled = true;

    // Only for display and for re-binding a preset to a freshly loaded plugin.
    // Never used to address the parameter at render time.
    std::string nodeName;
    std::string paramName;
};

class ColorNode : public Node {
public:
    ColorNode();

    // -- targets (message thread) ------------------------------------------
    const std::vector<ColorTarget>& targets() const noexcept { return targets_; }
    void addTarget(ParamAddress address, const Graph& graph);
    void removeTarget(int index);
    void setTarget(int index, const ColorTarget& target);
    void clearTargets();
    bool hasTarget(ParamAddress address) const;

    // -- capture (message thread) ------------------------------------------
    // Reads every target's current value out of the graph into one end of the
    // axis. This is how a colour preset is made: set the chain up by ear, then
    // press the end you meant.
    enum class End { Red, Neutral, Blue };
    void captureEnd(End end, const Graph& graph);

    // Adds every automatable parameter of `node` as a target, with all three
    // ends set to whatever it currently reads. A chain starts neutral, so
    // nothing moves until an end is captured.
    int adoptNode(NodeId node, const Graph& graph);

    // -- presets (message thread) ------------------------------------------
    // Presets store parameter *names*, not indices, and re-bind by matching
    // them against whatever the plugin reports now. Anything that does not match
    // is dropped and counted rather than silently pointed at the wrong control.
    JsonValue savePreset(const std::string& name, const Graph& graph) const;
    // Returns how many targets bound, and fills `unmatched` with the names that
    // did not. Existing targets are replaced.
    int loadPreset(const JsonValue& preset, const Graph& graph,
                   std::vector<std::string>* unmatched = nullptr);

    // -- performance -------------------------------------------------------
    // -1 red .. 0 neutral .. +1 blue.
    float color() const noexcept;
    void setColor(float c) noexcept;

    // The value this node last wrote for a target, for the UI to draw.
    float lastWrittenValue(int index) const;

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

    // The colour node needs to reach parameters on other nodes, which is not
    // something an ordinary node does. The graph hands it a pointer once, on
    // prepare, rather than every node carrying one.
    void setOwningGraph(Graph* graph) override { graph_ = graph; }

    static constexpr const char* kColorParam = "color";

private:
    // Applies the current colour to every enabled target. Runs on the message
    // thread, not the audio thread: writing another node's parameter is a store
    // to an atomic that the owning node reads next block, so it does not need to
    // happen in the render call - and doing it there would mean one node reaching
    // into another's parameters while that node is mid-block.
    void applyColor();

    Graph* graph_ = nullptr;

    std::vector<ColorTarget> targets_;
    std::vector<float> lastWritten_;

    ParamIndex pColor_ = -1, pDepth_ = -1, pEnabled_ = -1;

    // The last colour written out, so an unchanged knob does not fight anyone
    // turning a plugin's own control by hand. Out of range initially, so the
    // first apply always runs.
    float appliedColor_ = 2.0f;
};

} // namespace acm

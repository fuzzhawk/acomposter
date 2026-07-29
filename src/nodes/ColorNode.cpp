#include "ColorNode.h"

#include "../core/Graph.h"

#include <algorithm>
#include <cmath>

namespace acm {

ColorNode::ColorNode() : Node("color", NodeCategory::Effect) {
    // No audio ports. The colour node is a controller: it sits on the canvas
    // next to the chain it drives rather than in the signal path, so a chain can
    // be re-ordered without unpatching it and one colour node can drive plugins
    // that are not adjacent to each other.

    pColor_ = indexOfParameter(
        addFloatParam(kColorParam, "Colour", -1.0f, 1.0f, 0.0f).id());
    parameter(pColor_).setUnit("red / blue");

    // Scales every target at once, so a chain that is too dramatic can be pulled
    // back without recapturing either end.
    pDepth_ = indexOfParameter(addFloatParam("depth", "Depth", 0.0f, 1.0f, 1.0f).id());
    pEnabled_ = indexOfParameter(addBoolParam("enabled", "Enabled", true).id());
}

// ---------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------

bool ColorNode::hasTarget(ParamAddress address) const {
    return std::any_of(targets_.begin(), targets_.end(),
                       [&](const ColorTarget& t) { return t.address == address; });
}

void ColorNode::addTarget(ParamAddress address, const Graph& graph) {
    if (!address.valid() || hasTarget(address)) return;
    // Driving our own colour from our own colour is a loop with no useful
    // meaning, and is easy to do by accident from a "add every parameter" button.
    if (address.node == id()) return;

    const Node* node = graph.node(address.node);
    if (!node || address.param >= node->numParameters()) return;

    const Parameter& parameter = node->parameter(address.param);
    if (!parameter.automatable()) return;

    ColorTarget target;
    target.address = address;
    target.nodeName = node->name();
    target.paramName = parameter.name();

    // All three ends start where the parameter already is, so adding a target
    // never changes the sound. The ends are captured deliberately, afterwards.
    const float current = parameter.normalised();
    target.redValue = target.neutralValue = target.blueValue = current;

    targets_.push_back(std::move(target));
    lastWritten_.push_back(current);
}

void ColorNode::removeTarget(int index) {
    if (index < 0 || index >= static_cast<int>(targets_.size())) return;
    targets_.erase(targets_.begin() + index);
    lastWritten_.erase(lastWritten_.begin() + index);
}

void ColorNode::setTarget(int index, const ColorTarget& target) {
    if (index < 0 || index >= static_cast<int>(targets_.size())) return;
    targets_[static_cast<std::size_t>(index)] = target;
    appliedColor_ = 2.0f;   // force a re-apply so the change is audible at once
}

void ColorNode::clearTargets() {
    targets_.clear();
    lastWritten_.clear();
}

int ColorNode::adoptNode(NodeId nodeId, const Graph& graph) {
    const Node* node = graph.node(nodeId);
    if (!node) return 0;

    int added = 0;
    for (int i = 0; i < node->numParameters(); ++i) {
        const ParamAddress address{ nodeId, i };
        if (hasTarget(address)) continue;
        if (!node->parameter(i).automatable()) continue;

        const std::size_t before = targets_.size();
        addTarget(address, graph);
        if (targets_.size() > before) ++added;
    }
    return added;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

void ColorNode::captureEnd(End end, const Graph& graph) {
    for (ColorTarget& target : targets_) {
        const Node* node = graph.node(target.address.node);
        if (!node || target.address.param >= node->numParameters()) continue;

        const float value = node->parameter(target.address.param).normalised();
        switch (end) {
            case End::Red:     target.redValue = value; break;
            case End::Neutral: target.neutralValue = value; break;
            case End::Blue:    target.blueValue = value; break;
        }
    }
    appliedColor_ = 2.0f;
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

JsonValue ColorNode::savePreset(const std::string& name, const Graph& graph) const {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-color-preset");
    root.set("version", 1);
    root.set("name", name);

    JsonValue array = JsonValue::array();
    for (const ColorTarget& target : targets_) {
        const Node* node = graph.node(target.address.node);
        if (!node || target.address.param >= node->numParameters()) continue;

        JsonValue entry = JsonValue::object();
        // Written by name and by the node's type, never by index. A preset that
        // outlives a plugin update has to be able to find its parameters again.
        entry.set("node", node->name());
        entry.set("nodeType", node->typeName());
        entry.set("param", node->parameter(target.address.param).name());
        entry.set("paramId", node->parameter(target.address.param).id());
        entry.set("red", target.redValue);
        entry.set("neutral", target.neutralValue);
        entry.set("blue", target.blueValue);
        entry.set("depth", target.depth);
        entry.set("enabled", target.enabled);
        array.push(entry);
    }
    root.set("targets", array);
    return root;
}

int ColorNode::loadPreset(const JsonValue& preset, const Graph& graph,
                          std::vector<std::string>* unmatched) {
    const JsonValue* array = preset.find("targets");
    if (!array || !array->isArray()) return 0;

    clearTargets();
    int bound = 0;

    for (std::size_t i = 0; i < array->size(); ++i) {
        const JsonValue& entry = array->at(i);

        const std::string wantNode = entry.getString("node");
        const std::string wantType = entry.getString("nodeType");
        const std::string wantParam = entry.getString("param");
        const std::string wantParamId = entry.getString("paramId");

        // Find the node by name first, then fall back to the only node of the
        // right type. Naming two plugins the same is the user's business, but
        // one of a kind is the common case and worth resolving.
        const Node* match = nullptr;
        int typeMatches = 0;
        const Node* typeMatch = nullptr;

        for (const auto& candidate : graph.nodes()) {
            if (candidate->id() == id()) continue;
            if (!wantNode.empty() && candidate->name() == wantNode) { match = candidate.get(); break; }
            if (!wantType.empty() && candidate->typeName() == wantType) {
                ++typeMatches;
                typeMatch = candidate.get();
            }
        }
        if (!match && typeMatches == 1) match = typeMatch;

        ParamIndex index = -1;
        if (match) {
            // The stable id is tried before the display name: a plugin may
            // rename a control between versions but keeps its slot id.
            if (!wantParamId.empty()) index = match->indexOfParameter(wantParamId);
            if (index < 0) {
                for (int p = 0; p < match->numParameters(); ++p) {
                    if (match->parameter(p).name() == wantParam) { index = p; break; }
                }
            }
        }

        if (!match || index < 0) {
            if (unmatched) unmatched->push_back(wantNode + " / " + wantParam);
            continue;
        }

        ColorTarget target;
        target.address = ParamAddress{ match->id(), index };
        target.nodeName = match->name();
        target.paramName = match->parameter(index).name();
        target.redValue = entry.getFloat("red", 0.5f);
        target.neutralValue = entry.getFloat("neutral", 0.5f);
        target.blueValue = entry.getFloat("blue", 0.5f);
        target.depth = entry.getFloat("depth", 1.0f);
        target.enabled = entry.getBool("enabled", true);

        targets_.push_back(std::move(target));
        lastWritten_.push_back(0.5f);
        ++bound;
    }

    appliedColor_ = 2.0f;
    return bound;
}

// ---------------------------------------------------------------------------
// Performance
// ---------------------------------------------------------------------------

float ColorNode::color() const noexcept {
    return pColor_ >= 0 ? paramValue(pColor_) : 0.0f;
}

void ColorNode::setColor(float c) noexcept {
    if (pColor_ >= 0) parameter(pColor_).setValue(clampValue(c, -1.0f, 1.0f));
}

float ColorNode::lastWrittenValue(int index) const {
    if (index < 0 || index >= static_cast<int>(lastWritten_.size())) return 0.0f;
    return lastWritten_[static_cast<std::size_t>(index)];
}

void ColorNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);
    appliedColor_ = 2.0f;
}

void ColorNode::process(ProcessContext& ctx) {
    // Nothing to render: this node is a controller, and the work happens on the
    // message thread where writing another node's parameter is safe.
    ctx.clearOutputs();
}

void ColorNode::serviceFromMessageThread() {
    applyColor();
}

void ColorNode::applyColor() {
    if (!graph_ || targets_.empty()) return;
    if (pEnabled_ >= 0 && paramValue(pEnabled_) <= 0.5f) return;

    // Read straight from the parameter rather than from anything the audio
    // thread publishes. The value is an atomic that a metasurface blend, a MIDI
    // knob and the inspector all write directly, and routing it through the
    // render call would mean the colour did nothing at all with the transport
    // stopped - which is exactly when a preset is being built.
    const float c = clampValue(color(), -1.0f, 1.0f);
    const float masterDepth = pDepth_ >= 0 ? paramValue(pDepth_) : 1.0f;

    // Only write when something moved. Every write is a store another node reads
    // next block, and writing a value it already holds each frame would fight
    // anyone turning the plugin's own knob by hand.
    if (std::abs(c - appliedColor_) < 1.0e-5f) return;
    appliedColor_ = c;

    for (std::size_t i = 0; i < targets_.size(); ++i) {
        ColorTarget& target = targets_[i];
        if (!target.enabled) continue;

        Node* node = graph_->node(target.address.node);
        if (!node || target.address.param >= node->numParameters()) continue;

        // Two half-axes rather than one, so the middle is exactly the neutral
        // capture. A single lerp from red to blue would only pass through
        // neutral if neutral happened to be their average, which for anything
        // asymmetric - a filter that opens one way and a reverb that grows the
        // other - it is not.
        const float end = c < 0.0f ? target.redValue : target.blueValue;
        const float amount = std::abs(c) * target.depth * masterDepth;
        const float value = target.neutralValue + (end - target.neutralValue) * amount;

        lastWritten_[i] = value;
        node->parameter(target.address.param).setNormalised(value);
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void ColorNode::saveExtraState(JsonValue& out) const {
    JsonValue array = JsonValue::array();
    for (const ColorTarget& target : targets_) {
        JsonValue entry = JsonValue::object();
        // Inside a patch the node ids are stable, so the address is written
        // directly - unlike a preset, which has to survive being loaded into a
        // different patch entirely.
        entry.set("node", static_cast<int>(target.address.node));
        entry.set("param", static_cast<int>(target.address.param));
        entry.set("nodeName", target.nodeName);
        entry.set("paramName", target.paramName);
        entry.set("red", target.redValue);
        entry.set("neutral", target.neutralValue);
        entry.set("blue", target.blueValue);
        entry.set("depth", target.depth);
        entry.set("enabled", target.enabled);
        array.push(entry);
    }
    out.set("targets", array);
}

void ColorNode::loadExtraState(const JsonValue& in) {
    clearTargets();

    const JsonValue* array = in.find("targets");
    if (!array || !array->isArray()) return;

    for (std::size_t i = 0; i < array->size(); ++i) {
        const JsonValue& entry = array->at(i);

        ColorTarget target;
        target.address = ParamAddress{ static_cast<NodeId>(entry.getInt("node", -1)),
                                       static_cast<ParamIndex>(entry.getInt("param", -1)) };
        if (!target.address.valid()) continue;

        target.nodeName = entry.getString("nodeName");
        target.paramName = entry.getString("paramName");
        target.redValue = entry.getFloat("red", 0.5f);
        target.neutralValue = entry.getFloat("neutral", 0.5f);
        target.blueValue = entry.getFloat("blue", 0.5f);
        target.depth = entry.getFloat("depth", 1.0f);
        target.enabled = entry.getBool("enabled", true);

        targets_.push_back(std::move(target));
        lastWritten_.push_back(target.neutralValue);
    }

    appliedColor_ = 2.0f;
}

} // namespace acm

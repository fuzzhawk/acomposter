#include "Node.h"

namespace acm {

const char* toString(NodeCategory c) noexcept {
    switch (c) {
        case NodeCategory::Source:   return "source";
        case NodeCategory::Effect:   return "effect";
        case NodeCategory::Mixing:   return "mixing";
        case NodeCategory::Routing:  return "routing";
        case NodeCategory::Output:   return "output";
        case NodeCategory::Analysis: return "analysis";
    }
    return "effect";
}

Node::Node(std::string typeName, NodeCategory category)
    : typeName_(std::move(typeName)), category_(category) {
    name_ = typeName_;
}

Node::~Node() = default;

void Node::prepare(const PrepareInfo& info) {
    sampleRate_ = info.sampleRate;
    maxBlockSize_ = info.maxBlockSize;
}

void Node::addInput(std::string name, int channels, bool sidechain) {
    PortDescriptor d;
    d.name = std::move(name);
    d.channels = clampValue(channels, 1, kMaxChannelsPerPort);
    d.sidechain = sidechain;
    inputPorts_.push_back(std::move(d));
}

void Node::addOutput(std::string name, int channels) {
    PortDescriptor d;
    d.name = std::move(name);
    d.channels = clampValue(channels, 1, kMaxChannelsPerPort);
    outputPorts_.push_back(std::move(d));
}

Parameter& Node::addParameter(std::string id, std::string name, ParamKind kind,
                              float minValue, float maxValue, float defaultValue) {
    params_.push_back(std::make_unique<Parameter>(std::move(id), std::move(name), kind,
                                                  minValue, maxValue, defaultValue));
    return *params_.back();
}

Parameter& Node::addFloatParam(std::string id, std::string name, float lo, float hi, float def) {
    return addParameter(std::move(id), std::move(name), ParamKind::Float, lo, hi, def);
}

Parameter& Node::addDbParam(std::string id, std::string name, float lo, float hi, float def) {
    auto& p = addParameter(std::move(id), std::move(name), ParamKind::Float, lo, hi, def);
    p.setCurve(ParamCurve::Decibels).setUnit("dB");
    return p;
}

Parameter& Node::addBoolParam(std::string id, std::string name, bool def) {
    return addParameter(std::move(id), std::move(name), ParamKind::Bool, 0.0f, 1.0f, def ? 1.0f : 0.0f);
}

Parameter& Node::addChoiceParam(std::string id, std::string name,
                                std::vector<std::string> choices, int def) {
    auto& p = addParameter(std::move(id), std::move(name), ParamKind::Choice,
                           0.0f, static_cast<float>(choices.empty() ? 0 : choices.size() - 1),
                           static_cast<float>(def));
    p.setChoices(std::move(choices));
    p.setValue(static_cast<float>(def));
    return p;
}

Parameter& Node::addIntParam(std::string id, std::string name, int lo, int hi, int def) {
    return addParameter(std::move(id), std::move(name), ParamKind::Int,
                        static_cast<float>(lo), static_cast<float>(hi), static_cast<float>(def));
}

Parameter* Node::findParameter(std::string_view id) {
    for (auto& p : params_)
        if (p->id() == id) return p.get();
    return nullptr;
}

const Parameter* Node::findParameter(std::string_view id) const {
    for (const auto& p : params_)
        if (p->id() == id) return p.get();
    return nullptr;
}

ParamIndex Node::indexOfParameter(std::string_view id) const {
    for (std::size_t i = 0; i < params_.size(); ++i)
        if (params_[i]->id() == id) return static_cast<ParamIndex>(i);
    return -1;
}

} // namespace acm

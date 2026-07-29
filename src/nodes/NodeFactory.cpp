#include "NodeFactory.h"

#include "BuildNode.h"
#include "ColorNode.h"
#include "CrossfaderNode.h"
#include "IoNodes.h"
#include "LooperNode.h"
#include "MixerNode.h"
#include "SamplePlayerNode.h"
#include "StemPlayerNode.h"
#include "UtilityNodes.h"

#include <algorithm>

namespace acm {

NodeFactory& NodeFactory::instance() {
    static NodeFactory factory;
    return factory;
}

void NodeFactory::registerType(NodeTypeInfo info) {
    // Re-registering a type replaces it, so a plugin rescan cannot leave two
    // entries with the same key in the palette.
    const auto it = std::find_if(types_.begin(), types_.end(),
                                 [&](const NodeTypeInfo& t) { return t.typeName == info.typeName; });
    if (it != types_.end())
        *it = std::move(info);
    else
        types_.push_back(std::move(info));
}

const NodeTypeInfo* NodeFactory::find(std::string_view typeName) const {
    for (const auto& t : types_)
        if (t.typeName == typeName) return &t;
    return nullptr;
}

std::unique_ptr<Node> NodeFactory::create(std::string_view typeName) const {
    const NodeTypeInfo* info = find(typeName);
    if (!info || !info->create) return nullptr;
    return info->create();
}

std::unique_ptr<Node> NodeFactory::createExternal(const std::string& typeName,
                                                  const JsonValue& state) const {
    if (!externalLoader_) return nullptr;
    return externalLoader_(typeName, state);
}

// ---------------------------------------------------------------------------

void registerBuiltinNodes() {
    NodeFactory& factory = NodeFactory::instance();

    const auto add = [&](const char* typeName, const char* displayName, const char* description,
                         NodeCategory category, const char* group, int sortOrder,
                         std::function<std::unique_ptr<Node>()> create) {
        NodeTypeInfo info;
        info.typeName = typeName;
        info.displayName = displayName;
        info.description = description;
        info.category = category;
        info.paletteGroup = group;
        info.sortOrder = sortOrder;
        info.create = std::move(create);
        factory.registerType(std::move(info));
    };

    // -- sources -----------------------------------------------------------

    add("sample.player", "Sample Player",
        "Loops or triggers an audio file, locked to the transport or free running.",
        NodeCategory::Source, "Sources", 10,
        [] { return std::make_unique<SamplePlayerNode>(); });

    add("stem.player", "Stem Player",
        "Plays a song's stems together and loops a named section of it, switching "
        "sections on the grid.",
        NodeCategory::Source, "Sources", 15,
        [] { return std::make_unique<StemPlayerNode>(); });

    add("looper", "Looper",
        "Records a live loop and layers overdubs onto it.",
        NodeCategory::Source, "Sources", 20,
        [] { return std::make_unique<LooperNode>(); });

    add("util.tone", "Tone",
        "Band-limited oscillator and noise source.",
        NodeCategory::Source, "Sources", 30,
        [] { return std::make_unique<ToneNode>(); });

    add("io.in", "Audio In",
        "Brings the capture stream from the audio device into the patch.",
        NodeCategory::Source, "Sources", 40,
        [] { return std::make_unique<AudioInNode>(); });

    // -- controllers -------------------------------------------------------
    // These drive other nodes' parameters rather than carrying audio, which is
    // why they sit in their own palette group instead of among the effects.

    add("color", "Colour",
        "Moves a whole effect chain between two captured states with one knob, "
        "through an untouched middle.",
        NodeCategory::Effect, "Controllers", 10,
        [] { return std::make_unique<ColorNode>(); });

    add("build", "Build",
        "A momentary switch that stutters the loop, runs a riser and pushes the "
        "colour, then drops back on the grid.",
        NodeCategory::Effect, "Controllers", 20,
        [] { return std::make_unique<BuildNode>(); });

    // -- mixing ------------------------------------------------------------

    add("crossfader", "Crossfader",
        "Blends two sources with selectable curve laws and hard cuts.",
        NodeCategory::Mixing, "Mixing", 10,
        [] { return std::make_unique<CrossfaderNode>(); });

    for (int width : { 4, 8, 16 }) {
        const std::string typeName = MixerNode::typeNameForWidth(width);
        const std::string displayName = "Mixer " + std::to_string(width);
        NodeTypeInfo info;
        info.typeName = typeName;
        info.displayName = displayName;
        info.description = std::to_string(width) + " stereo channels with gain, pan, mute and solo.";
        info.category = NodeCategory::Mixing;
        info.paletteGroup = "Mixing";
        info.sortOrder = 20 + width;
        info.create = [width] { return std::make_unique<MixerNode>(width); };
        factory.registerType(std::move(info));
    }

    add("util.gain", "Gain",
        "Level, pan, polarity and DC removal.",
        NodeCategory::Mixing, "Mixing", 60,
        [] { return std::make_unique<GainNode>(); });

    // -- effects and analysis ---------------------------------------------

    add("util.filter", "Filter",
        "Biquad filter with seven responses and a wet/dry blend.",
        NodeCategory::Effect, "Effects", 10,
        [] { return std::make_unique<FilterNode>(); });

    add("util.monitor", "Monitor",
        "Passes audio through and reports peak, RMS and clipping.",
        NodeCategory::Analysis, "Analysis", 10,
        [] { return std::make_unique<MonitorNode>(); });

    // -- output ------------------------------------------------------------

    add("io.out", "Audio Out",
        "Sums into the master output bus.",
        NodeCategory::Output, "Output", 10,
        [] { return std::make_unique<AudioOutNode>(); });
}

} // namespace acm

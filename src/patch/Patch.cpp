#include "Patch.h"

#include "../core/FileIo.h"
#include "../nodes/CrossfaderNode.h"
#include "../nodes/IoNodes.h"
#include "../nodes/LooperNode.h"
#include "../nodes/MixerNode.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/SamplePlayerNode.h"

#include <algorithm>

namespace acm::patch {
namespace {

JsonValue saveNode(const Node& node) {
    JsonValue out = JsonValue::object();
    out.set("id", static_cast<double>(node.id()));
    out.set("type", node.typeName());
    out.set("name", node.name());
    out.set("x", node.canvasX);
    out.set("y", node.canvasY);
    if (node.canvasWidth > 0.0f) out.set("width", node.canvasWidth);
    if (node.collapsed) out.set("collapsed", true);
    if (node.bypassed()) out.set("bypassed", true);
    if (!node.comment.empty()) out.set("comment", node.comment);

    // Parameters are written by their stable string id and in real units, so a
    // patch stays readable and survives a parameter being reordered.
    JsonValue params = JsonValue::object();
    for (int i = 0; i < node.numParameters(); ++i) {
        const Parameter& parameter = node.parameter(i);
        params.set(parameter.id(), parameter.value());
    }
    out.set("params", params);

    JsonValue extra = JsonValue::object();
    node.saveExtraState(extra);
    if (!extra.members().empty()) out.set("state", extra);

    return out;
}

void applyNodeCommon(Node& node, const JsonValue& in) {
    node.setName(in.getString("name", node.name()));
    node.canvasX = in.getFloat("x", 0.0f);
    node.canvasY = in.getFloat("y", 0.0f);
    node.canvasWidth = in.getFloat("width", 0.0f);
    node.collapsed = in.getBool("collapsed", false);
    node.setBypassed(in.getBool("bypassed", false));
    node.comment = in.getString("comment", "");

    if (const JsonValue* params = in.find("params")) {
        for (const auto& kv : params->members()) {
            if (Parameter* parameter = node.findParameter(kv.first))
                parameter->setValue(kv.second.asFloat(parameter->defaultValue()));
            // Unknown parameter ids are ignored on purpose: a patch saved by a
            // later version should still open here, minus what we do not know.
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------

JsonValue save(const Engine& engine, const Metasurface& metasurface,
               const control::Surface& surface,
               const PatchViewState& view, const PatchMetadata& metadata) {
    JsonValue root = JsonValue::object();
    root.set("format", kFormatId);
    root.set("version", kFormatVersion);
    root.set("application", std::string("acomposter ") + ACOMPOSTER_VERSION);

    if (!metadata.title.empty() || !metadata.author.empty() || !metadata.notes.empty()) {
        JsonValue meta = JsonValue::object();
        if (!metadata.title.empty()) meta.set("title", metadata.title);
        if (!metadata.author.empty()) meta.set("author", metadata.author);
        if (!metadata.notes.empty()) meta.set("notes", metadata.notes);
        root.set("metadata", meta);
    }

    const TransportState transport = engine.transport().snapshot();
    JsonValue transportObject = JsonValue::object();
    transportObject.set("bpm", transport.bpm);
    transportObject.set("timeSigNumerator", transport.timeSigNumerator);
    transportObject.set("timeSigDenominator", transport.timeSigDenominator);
    transportObject.set("loopEnabled", transport.loopEnabled);
    transportObject.set("loopStartPpq", transport.loopStartPpq);
    transportObject.set("loopEndPpq", transport.loopEndPpq);
    root.set("transport", transportObject);

    JsonValue master = JsonValue::object();
    master.set("gainDb", engine.masterGainDb());
    master.set("muted", engine.masterMuted());
    master.set("limiter", engine.masterLimiterEnabled());
    root.set("master", master);

    JsonValue nodes = JsonValue::array();
    for (const auto& node : engine.graph().nodes())
        nodes.push(saveNode(*node));
    root.set("nodes", nodes);

    JsonValue connections = JsonValue::array();
    for (const Connection& c : engine.graph().connections()) {
        JsonValue entry = JsonValue::object();
        entry.set("from", static_cast<double>(c.sourceNode));
        entry.set("fromPort", c.sourcePort);
        entry.set("to", static_cast<double>(c.destNode));
        entry.set("toPort", c.destPort);
        connections.push(entry);
    }
    root.set("connections", connections);

    root.set("metasurface", metasurface.toJson());
    root.set("surface", surface.toJson());

    JsonValue viewObject = JsonValue::object();
    viewObject.set("canvasX", view.canvasX);
    viewObject.set("canvasY", view.canvasY);
    viewObject.set("zoom", view.zoom);
    viewObject.set("activeView", view.activeView);
    root.set("view", viewObject);

    return root;
}

bool saveToFile(const std::string& utf8Path, const Engine& engine,
                const Metasurface& metasurface, const control::Surface& surface,
                const PatchViewState& view,
                const PatchMetadata& metadata, std::string* error) {
    const JsonValue root = save(engine, metasurface, surface, view, metadata);
    return writeFileText(utf8Path, root.dump(2), error);
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

PatchLoadResult load(const JsonValue& root, Engine& engine, Metasurface& metasurface,
                     control::Surface& surface,
                     PatchViewState& view, PatchMetadata& metadata) {
    PatchLoadResult result;

    if (!root.isObject()) {
        result.error = "patch file does not contain a JSON object";
        return result;
    }
    if (root.getString("format") != kFormatId) {
        result.error = "not an acomposter patch (missing or wrong \"format\" field)";
        return result;
    }

    const int version = root.getInt("version", 0);
    if (version > kFormatVersion) {
        result.warnings.push_back("this patch was written by a newer version of acomposter ("
                                  "format " + std::to_string(version) + "); anything unrecognised "
                                  "has been dropped");
    }

    Graph& graph = engine.graph();
    graph.clear();
    metasurface.clear();
    metasurface.clearPath();

    metadata = PatchMetadata{};
    if (const JsonValue* meta = root.find("metadata")) {
        metadata.title = meta->getString("title");
        metadata.author = meta->getString("author");
        metadata.notes = meta->getString("notes");
    }

    if (const JsonValue* transport = root.find("transport")) {
        engine.transport().setBpm(transport->getDouble("bpm", 120.0));
        engine.transport().setTimeSignature(transport->getInt("timeSigNumerator", 4),
                                            transport->getInt("timeSigDenominator", 4));
        engine.transport().setLoop(transport->getBool("loopEnabled", false),
                                   transport->getDouble("loopStartPpq", 0.0),
                                   transport->getDouble("loopEndPpq", 16.0));
    }

    if (const JsonValue* master = root.find("master")) {
        engine.setMasterGainDb(master->getFloat("gainDb", 0.0f));
        engine.setMasterMuted(master->getBool("muted", false));
        engine.setMasterLimiterEnabled(master->getBool("limiter", true));
    }

    // -- nodes -------------------------------------------------------------

    NodeFactory& factory = NodeFactory::instance();

    if (const JsonValue* nodes = root.find("nodes")) {
        for (const JsonValue& entry : nodes->items()) {
            const auto id = static_cast<NodeId>(entry.getInt64("id", 0));
            const std::string type = entry.getString("type");

            if (id == kInvalidNode || type.empty()) {
                result.warnings.push_back("skipped a node with no id or type");
                continue;
            }

            const JsonValue* state = entry.find("state");
            const JsonValue emptyState = JsonValue::object();

            std::unique_ptr<Node> node = factory.create(type);
            if (!node) {
                // Plugin nodes are not in the static registry; the plugin host
                // installs a loader that can reconstruct them from their state.
                node = factory.createExternal(type, state ? *state : emptyState);
            }

            if (!node) {
                result.warnings.push_back("node type \"" + type + "\" is not available; "
                                          "that node and its connections were dropped");
                continue;
            }

            applyNodeCommon(*node, entry);
            if (state) node->loadExtraState(*state);

            if (graph.addNodeWithId(std::move(node), id) == kInvalidNode)
                result.warnings.push_back("duplicate node id " + std::to_string(id) + " was skipped");
        }
    }

    // -- connections -------------------------------------------------------

    if (const JsonValue* connections = root.find("connections")) {
        for (const JsonValue& entry : connections->items()) {
            const auto from = static_cast<NodeId>(entry.getInt64("from", 0));
            const auto to = static_cast<NodeId>(entry.getInt64("to", 0));
            const auto fromPort = static_cast<PortIndex>(entry.getInt("fromPort", 0));
            const auto toPort = static_cast<PortIndex>(entry.getInt("toPort", 0));

            std::string reason;
            if (!graph.canConnect(from, fromPort, to, toPort, &reason)) {
                result.warnings.push_back("dropped a connection (" + reason + ")");
                continue;
            }
            graph.connect(from, fromPort, to, toPort);
        }
    }

    // -- metasurface -------------------------------------------------------

    if (const JsonValue* meta = root.find("metasurface")) {
        metasurface.fromJson(*meta);
        // A patch can legitimately reference parameters whose node failed to
        // load; drop those rather than carrying dead weight.
        metasurface.pruneMissing(graph);
    }

    // -- control surface ---------------------------------------------------
    // Cleared either way: a patch without a surface has to open with an empty
    // one rather than with the last patch's layout still bound to node ids that
    // now mean something else entirely.
    surface.clear();
    if (const JsonValue* surfaceObject = root.find("surface")) {
        surface.fromJson(*surfaceObject);
        surface.pruneMissing(graph);
    }

    if (const JsonValue* viewObject = root.find("view")) {
        view.canvasX = viewObject->getFloat("canvasX", 0.0f);
        view.canvasY = viewObject->getFloat("canvasY", 0.0f);
        view.zoom = clampValue(viewObject->getFloat("zoom", 1.0f), 0.2f, 4.0f);
        view.activeView = viewObject->getInt("activeView", 0);
    }

    result.ok = true;
    return result;
}

PatchLoadResult loadFromFile(const std::string& utf8Path, Engine& engine,
                             Metasurface& metasurface, control::Surface& surface,
                             PatchViewState& view, PatchMetadata& metadata) {
    PatchLoadResult result;

    std::string text;
    if (!readFileText(utf8Path, text, &result.error)) return result;

    std::string parseError;
    const JsonValue root = JsonValue::parse(text, &parseError);
    if (!parseError.empty()) {
        result.error = "could not parse patch: " + parseError;
        return result;
    }

    return load(root, engine, metasurface, surface, view, metadata);
}

// ---------------------------------------------------------------------------
// Default patch
// ---------------------------------------------------------------------------

void buildDefaultPatch(Engine& engine, Metasurface& metasurface) {
    Graph& graph = engine.graph();
    graph.clear();
    metasurface.clear();

    const auto place = [&](NodeId id, float x, float y) {
        if (Node* node = graph.node(id)) { node->canvasX = x; node->canvasY = y; }
    };

    auto player = std::make_unique<SamplePlayerNode>();
    player->setName("Deck A");
    const NodeId deckA = graph.addNode(std::move(player));

    auto looper = std::make_unique<LooperNode>();
    looper->setName("Deck B");
    const NodeId deckB = graph.addNode(std::move(looper));

    const NodeId crossfader = graph.addNode(std::make_unique<CrossfaderNode>());
    const NodeId mixer = graph.addNode(std::make_unique<MixerNode>(4));
    const NodeId input = graph.addNode(std::make_unique<AudioInNode>());
    const NodeId output = graph.addNode(std::make_unique<AudioOutNode>());

    place(deckA, 60.0f, 80.0f);
    place(deckB, 60.0f, 330.0f);
    place(input, 60.0f, 570.0f);
    place(crossfader, 420.0f, 190.0f);
    place(mixer, 700.0f, 240.0f);
    place(output, 1000.0f, 300.0f);

    graph.connect(deckA, 0, crossfader, 0);
    graph.connect(deckB, 0, crossfader, 1);
    graph.connect(crossfader, 0, mixer, 0);
    graph.connect(input, 0, mixer, 1);
    graph.connect(mixer, 0, output, 0);

    // The looper listens to the live input by default, so arming record does
    // something useful without any further patching.
    graph.connect(input, 0, deckB, 0);

    // Two snapshots on opposite corners give the metasurface something to
    // interpolate between the first time it is opened.
    if (Node* fader = graph.node(crossfader)) {
        if (Parameter* position = fader->findParameter("position")) {
            position->setValue(0.0f);
            metasurface.capture(graph, "A", Point2{ 0.2f, 0.75f });
            position->setValue(1.0f);
            metasurface.capture(graph, "B", Point2{ 0.8f, 0.25f });
            position->setValue(0.5f);
        }
    }
}

} // namespace acm::patch

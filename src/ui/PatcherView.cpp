#include "PatcherView.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"
#include "../nodes/CrossfaderNode.h"
#include "../nodes/LooperNode.h"
#include "../nodes/MixerNode.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/BuildNode.h"
#include "../nodes/DropNode.h"
#include "../nodes/ColorNode.h"
#include "../nodes/SamplePlayerNode.h"
#include "../nodes/StemPlayerNode.h"
#include "../vst2/PluginManager.h"
#include "../vst2/VstNode.h"

#include <algorithm>
#include <cmath>

namespace acm::ui {
namespace {

constexpr float kPortSpacing = 20.0f;
constexpr float kPortInset = 0.0f;
constexpr float kResizeHandleWidth = 8.0f;
constexpr float kGridSpacing = 24.0f;

// The canvas keeps its own copies of the four metrics it needs, because it is
// the one place in the app that works in design units rather than device
// pixels: everything here is multiplied by viewScale() on the way to the
// screen, and the theme's metrics have already had the display scale folded in.
// Reading them here would apply it twice.
constexpr float kHeaderHeight = 24.0f;
constexpr float kPortRadius = 5.0f;
constexpr float kCableThickness = 2.0f;
constexpr float kNodeMinWidth = 168.0f;

// The vertical layout of a stem player's body, in design units.
//
// Named here because two places need it and they are nowhere near each other:
// drawStemPlayerBody, which lays the rows out, and handleFileDrop, which has to
// work out which stem strip a dropped file landed on. They were separate
// arithmetic and they drifted - the tempo row grew from 18 to 20 and the strips
// from 17 to 23 - so by the time it was noticed, dropping a file on the sixth
// stem loaded the fourth. The error is in design units and multiplied by the
// view scale, which is why it looked like a zoom bug: at 2.5x it was more than
// two strips.
namespace stemBody {
constexpr float kSections = 76.0f;
constexpr float kAfterSections = 4.0f;
constexpr float kControlRow = 18.0f;
constexpr float kAfterControls = 3.0f;
constexpr float kTempoRow = 20.0f;
constexpr float kAfterTempo = 4.0f;
constexpr float kStripHeight = 23.0f;

// From the top of the body to the top of the first strip.
constexpr float stripsOffset() {
    return kSections + kAfterSections + kControlRow + kAfterControls
         + kTempoRow + kAfterTempo;
}
} // namespace stemBody

// The drop node's vertical layout, for the same reason as above: the file drop
// has to know which of the three layers it landed on.
namespace dropBody {
constexpr float kFireRow = 30.0f;
constexpr float kAfterFire = 5.0f;
constexpr float kLayerHeight = 40.0f;
constexpr float layersOffset() { return kFireRow + kAfterFire; }
} // namespace dropBody

// How far a cable's control points reach horizontally. Proportional to the
// span, so short hops stay tight and long runs sweep.
float cableTension(Vec2 from, Vec2 to) {
    const float distance = std::fabs(to.x - from.x);
    return clampValue(distance * 0.5f, 30.0f, 160.0f);
}

// Distance from a point to a cubic Bezier, sampled. Used for cable hit testing,
// where exactness matters far less than "did I click near this line".
float distanceToBezier(Vec2 p, Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3) {
    constexpr int kSamples = 24;
    float best = 1.0e9f;
    Vec2 previous = p0;

    for (int i = 1; i <= kSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSamples);
        const float u = 1.0f - t;
        const Vec2 current{
            p0.x * u * u * u + p1.x * 3.0f * u * u * t + p2.x * 3.0f * u * t * t + p3.x * t * t * t,
            p0.y * u * u * u + p1.y * 3.0f * u * u * t + p2.y * 3.0f * u * t * t + p3.y * t * t * t
        };

        // Point-to-segment distance.
        const Vec2 segment = current - previous;
        const float lengthSquared = segment.lengthSquared();
        float projection = 0.0f;
        if (lengthSquared > 1.0e-6f)
            projection = clampValue(((p - previous).x * segment.x + (p - previous).y * segment.y)
                                        / lengthSquared, 0.0f, 1.0f);
        const Vec2 closest = previous + segment * projection;
        best = std::min(best, (p - closest).length());

        previous = current;
    }
    return best;
}

} // namespace

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void PatcherView::initialise(Engine* engine, Metasurface* metasurface,
                             vst2::PluginManager* plugins) {
    engine_ = engine;
    metasurface_ = metasurface;
    plugins_ = plugins;
}

float PatcherView::viewScale() const noexcept {
    return zoom_ * theme().scale;
}

Vec2 PatcherView::screenToWorld(Vec2 screen, const Rect& bounds) const {
    const float z = viewScale();
    return { (screen.x - bounds.left()) / z - pan_.x,
             (screen.y - bounds.top()) / z - pan_.y };
}

Vec2 PatcherView::worldToScreen(Vec2 world, const Rect& bounds) const {
    const float z = viewScale();
    return { (world.x + pan_.x) * z + bounds.left(),
             (world.y + pan_.y) * z + bounds.top() };
}

void PatcherView::resetView() {
    pan_ = { 40.0f, 40.0f };
    zoom_ = 1.0f;
}

void PatcherView::frameAll(const Rect& bounds) {
    if (!engine_ || engine_->graph().nodeCount() == 0) { resetView(); return; }

    float minX = 1.0e9f, minY = 1.0e9f, maxX = -1.0e9f, maxY = -1.0e9f;
    for (const auto& node : engine_->graph().nodes()) {
        minX = std::min(minX, node->canvasX);
        minY = std::min(minY, node->canvasY);
        maxX = std::max(maxX, node->canvasX + nodeWidth(*node));
        maxY = std::max(maxY, node->canvasY + nodeHeight(*node));
    }

    const float contentWidth = std::max(1.0f, maxX - minX);
    const float contentHeight = std::max(1.0f, maxY - minY);
    const float margin = 60.0f;

    // The fit is a screen measurement, so it produces a view scale; the zoom the
    // user sees and saves is that with the display scale divided back out.
    const float fitted = std::min((bounds.width - margin) / contentWidth,
                                  (bounds.height - margin) / contentHeight);
    zoom_ = clampValue(fitted / std::max(0.01f, theme().scale), kMinZoom, 1.0f);

    // Centre the content in the view.
    const float z = viewScale();
    pan_ = { (bounds.width / z - contentWidth) * 0.5f - minX,
             (bounds.height / z - contentHeight) * 0.5f - minY };
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

float PatcherView::nodeWidth(const Node& node) const {
    if (node.canvasWidth > 0.0f) return node.canvasWidth;

    if (node.typeName() == "sample.player" || node.typeName() == "looper") return 230.0f;
    // Wide enough for a row of section buttons and a column of stem strips.
    if (node.typeName() == "stem.player") {
        // The matrix hangs off the right rather than replacing anything, so the
        // strips stay readable while routing is being changed.
        const auto& stems = static_cast<const StemPlayerNode&>(node);
        return stems.matrixOpen ? 460.0f + 250.0f : 460.0f;
    }
    if (node.typeName() == "color") return 260.0f;
    if (node.typeName() == "build") return 320.0f;
    if (node.typeName() == "drop") return 250.0f;
    if (node.typeName().rfind("mixer.", 0) == 0) {
        const auto* mixer = static_cast<const MixerNode*>(&node);
        return clampValue(46.0f * static_cast<float>(mixer->channelCount()) + 20.0f, 180.0f, 780.0f);
    }
    if (node.typeName() == vst2::VstNode::kTypeName) return 220.0f;
    return kNodeMinWidth;
}

float PatcherView::nodeHeight(const Node& node) const {
    if (node.collapsed) return kHeaderHeight;

    // Tall enough for the ports, whatever the body wants.
    const float portHeight = static_cast<float>(std::max(node.numInputs(), node.numOutputs()))
                           * kPortSpacing;

    float bodyHeight = 64.0f;
    const std::string& type = node.typeName();

    if (type == "sample.player") bodyHeight = 128.0f;
    else if (type == "stem.player") bodyHeight = 360.0f;
    else if (type == "color") bodyHeight = 132.0f;
    else if (type == "build") bodyHeight = 292.0f;
    else if (type == "drop") bodyHeight = 192.0f;
    else if (type == "looper") bodyHeight = 116.0f;
    else if (type == "crossfader") bodyHeight = 92.0f;
    else if (type.rfind("mixer.", 0) == 0) bodyHeight = 132.0f;
    else if (type == vst2::VstNode::kTypeName) bodyHeight = 108.0f;
    else if (type == "io.out" || type == "io.in") bodyHeight = 56.0f;
    else {
        // Generic: a row per automatable parameter, capped so a plugin-like node
        // with forty parameters does not become a skyscraper.
        int rows = 0;
        for (int i = 0; i < node.numParameters(); ++i)
            if (node.parameter(i).automatable()) ++rows;
        bodyHeight = clampValue(static_cast<float>(rows) * 20.0f + 8.0f, 28.0f, 180.0f);
    }

    if (!node.errorText().empty()) bodyHeight += 18.0f;

    return kHeaderHeight + std::max(bodyHeight, portHeight + 8.0f);
}

Rect PatcherView::nodeBounds(const Node& node, const Rect& viewBounds) const {
    const float z = viewScale();
    const Vec2 topLeft = worldToScreen({ node.canvasX, node.canvasY }, viewBounds);
    return Rect{ topLeft.x, topLeft.y, nodeWidth(node) * z, nodeHeight(node) * z };
}

Vec2 PatcherView::portPosition(const Node& node, PortIndex port, bool isInput,
                               const Rect& viewBounds) const {
    const float z = viewScale();
    const Rect bounds = nodeBounds(node, viewBounds);

    const int count = isInput ? node.numInputs() : node.numOutputs();
    if (count <= 0) return bounds.centre();

    // Ports are distributed down the body, below the header.
    const float top = bounds.top() + kHeaderHeight * z;
    const float available = std::max(bounds.height - kHeaderHeight * z, 1.0f);
    const float step = available / static_cast<float>(count + 1);
    const float y = top + step * static_cast<float>(port + 1);

    return { isInput ? bounds.left() + kPortInset : bounds.right() - kPortInset, y };
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

NodeId PatcherView::hitTestNodes(Ui& ui, const Rect& viewBounds) const {
    if (!engine_) return kInvalidNode;

    const Vec2 pointer = ui.input().mousePosition;
    const auto& nodes = engine_->graph().nodes();

    // Reverse order so the node drawn last (on top) wins.
    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (nodeBounds(*nodes[i], viewBounds).contains(pointer))
            return nodes[i]->id();
    }
    return kInvalidNode;
}

// The node the pointer is over, in the order they are drawn: selected nodes are
// drawn last and so sit on top of unselected ones whatever the graph order.
NodeId PatcherView::topmostNodeAt(Vec2 pointer, const Rect& viewBounds) const {
    if (!engine_) return kInvalidNode;
    const auto& nodes = engine_->graph().nodes();

    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (!isSelected(nodes[i]->id())) continue;
        if (nodeBounds(*nodes[i], viewBounds).contains(pointer)) return nodes[i]->id();
    }
    for (std::size_t i = nodes.size(); i-- > 0;) {
        if (isSelected(nodes[i]->id())) continue;
        if (nodeBounds(*nodes[i], viewBounds).contains(pointer)) return nodes[i]->id();
    }
    return kInvalidNode;
}

PatcherView::PortHit PatcherView::hitTestPorts(Ui& ui, const Rect& viewBounds) const {
    const float z = viewScale();
    if (!engine_) return {};

    const Vec2 pointer = ui.input().mousePosition;
    // A generous radius: ports are small, and missing one mid-performance is
    // more annoying than occasionally grabbing the wrong one.
    const float radius = std::max(9.0f, kPortRadius * z * 1.8f);

    for (const auto& node : engine_->graph().nodes()) {
        for (int p = 0; p < node->numInputs(); ++p) {
            if ((portPosition(*node, p, true, viewBounds) - pointer).length() <= radius)
                return PortHit{ node->id(), p, true };
        }
        for (int p = 0; p < node->numOutputs(); ++p) {
            if ((portPosition(*node, p, false, viewBounds) - pointer).length() <= radius)
                return PortHit{ node->id(), p, false };
        }
    }
    return {};
}

ConnectionId PatcherView::hitTestCables(Ui& ui, const Rect& viewBounds) const {
    if (!engine_) return kInvalidConnection;

    const Vec2 pointer = ui.input().mousePosition;
    const Graph& graph = engine_->graph();

    for (const Connection& connection : graph.connections()) {
        const Node* source = graph.node(connection.sourceNode);
        const Node* destination = graph.node(connection.destNode);
        if (!source || !destination) continue;

        const Vec2 from = portPosition(*source, connection.sourcePort, false, viewBounds);
        const Vec2 to = portPosition(*destination, connection.destPort, true, viewBounds);
        const float tension = cableTension(from, to);

        if (distanceToBezier(pointer, from, { from.x + tension, from.y },
                             { to.x - tension, to.y }, to) < 6.0f)
            return connection.id;
    }
    return kInvalidConnection;
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

bool PatcherView::isSelected(NodeId node) const {
    return std::find(selection_.begin(), selection_.end(), node) != selection_.end();
}

void PatcherView::select(NodeId node, bool additive) {
    if (!additive) selection_.clear();
    if (node == kInvalidNode) return;

    const auto it = std::find(selection_.begin(), selection_.end(), node);
    if (it != selection_.end()) {
        if (additive) selection_.erase(it);   // shift-click toggles
    } else {
        selection_.push_back(node);
    }
}

void PatcherView::selectAll() {
    selection_.clear();
    if (!engine_) return;
    for (const auto& node : engine_->graph().nodes()) selection_.push_back(node->id());
}

void PatcherView::clearSelection() { selection_.clear(); }

NodeId PatcherView::focusedNode() const {
    if (!selection_.empty()) return selection_.back();
    return hovered_;
}

NodeId PatcherView::consumeEditorRequest() {
    const NodeId request = editorRequest_;
    editorRequest_ = kInvalidNode;
    return request;
}

PatcherView::RackRequest PatcherView::consumeRackRequest() {
    const RackRequest request = rackRequest_;
    rackRequest_ = RackRequest{};
    return request;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

Vec2 PatcherView::defaultDropPosition(const Rect& bounds) {
    const Vec2 centre = screenToWorld(bounds.centre(), bounds);
    // Cascade successive additions so they do not land exactly on top of one
    // another when several are made without moving the view.
    const float offset = static_cast<float>(newNodeCounter_ % 6) * 28.0f;
    return { centre.x - 90.0f + offset, centre.y - 60.0f + offset };
}

NodeId PatcherView::placeNode(std::unique_ptr<Node> node, Vec2 worldPosition) {
    if (!engine_ || !node) return kInvalidNode;

    node->canvasX = std::round(worldPosition.x);
    node->canvasY = std::round(worldPosition.y);

    const NodeId id = engine_->graph().addNode(std::move(node));
    if (id != kInvalidNode) {
        ++newNodeCounter_;
        selection_.clear();
        selection_.push_back(id);
    }
    return id;
}

NodeId PatcherView::addNodeAt(const std::string& typeName, Vec2 worldPosition) {
    auto node = NodeFactory::instance().create(typeName);
    if (!node) return kInvalidNode;
    return placeNode(std::move(node), worldPosition);
}

void PatcherView::deleteSelection() {
    if (!engine_) return;

    for (NodeId id : selection_) {
        // A plugin's editor window must go before the node that owns it.
        if (auto* plugin = dynamic_cast<vst2::VstNode*>(engine_->graph().node(id)))
            plugin->closeEditor();
        engine_->graph().removeNode(id);
    }
    selection_.clear();

    // Snapshots referring to the departed nodes would otherwise linger in the
    // patch file forever.
    if (metasurface_) metasurface_->pruneMissing(engine_->graph());
}

void PatcherView::duplicateSelection() {
    if (!engine_ || selection_.empty()) return;

    std::vector<NodeId> created;

    for (NodeId id : selection_) {
        const Node* original = engine_->graph().node(id);
        if (!original) continue;

        auto copy = NodeFactory::instance().create(original->typeName());
        // Plugin nodes cannot be rebuilt from the factory; duplicating one would
        // need a second instance of the plugin, which is a different operation.
        if (!copy) continue;

        copy->setName(original->name());
        copy->comment = original->comment;
        copy->canvasWidth = original->canvasWidth;
        copy->collapsed = original->collapsed;
        copy->setBypassed(original->bypassed());

        for (int p = 0; p < original->numParameters() && p < copy->numParameters(); ++p)
            copy->parameter(p).setValue(original->parameter(p).value());

        const NodeId newId = placeNode(std::move(copy),
                                       { original->canvasX + 32.0f, original->canvasY + 32.0f });
        if (newId != kInvalidNode) created.push_back(newId);
    }

    selection_ = created;
}

void PatcherView::bypassSelection() {
    if (!engine_) return;

    // Toggle as a group: if anything is live, bypass everything; otherwise
    // re-enable everything. Half-toggling a selection is never what is wanted.
    bool anyActive = false;
    for (NodeId id : selection_)
        if (const Node* node = engine_->graph().node(id))
            if (!node->bypassed()) { anyActive = true; break; }

    for (NodeId id : selection_)
        if (Node* node = engine_->graph().node(id)) node->setBypassed(anyActive);
}


// ---------------------------------------------------------------------------
// Stem effect chains
// ---------------------------------------------------------------------------

NodeId PatcherView::addToStemChain(NodeId stemPlayer, int stemSlot,
                                   const vst2::PluginDescription& description,
                                   bool forceBridge) {
    if (!engine_) return kInvalidNode;

    Graph& graph = engine_->graph();
    Node* player = graph.node(stemPlayer);
    if (!player || stemSlot < 0 || stemSlot >= player->numOutputs()) return kInvalidNode;

    // Where the chain currently ends, and what it currently feeds.
    const std::vector<NodeId> chain = downstreamChain(graph, stemPlayer, stemSlot);

    NodeId tailNode = stemPlayer;
    PortIndex tailPort = static_cast<PortIndex>(stemSlot);
    if (!chain.empty()) {
        tailNode = chain.back();
        tailPort = 0;
    }

    // Whatever the tail feeds now has to end up fed by the new plugin instead,
    // so inserting into a rack that is already going somewhere does not silence
    // it.
    std::vector<Connection> downstream;
    for (const Connection& c : graph.connections())
        if (c.sourceNode == tailNode && c.sourcePort == tailPort) downstream.push_back(c);

    auto node = std::make_unique<vst2::VstNode>(description);
    std::string error;
    const bool loaded = node->loadPlugin(*plugins_, engine_->stats().sampleRate,
                                         std::max(64, engine_->stats().blockSize),
                                         forceBridge, &error);

    const Node* playerNode = graph.node(stemPlayer);
    const float row = playerNode ? playerNode->canvasY + static_cast<float>(stemSlot) * 130.0f
                                 : 0.0f;
    const float column = playerNode
        ? playerNode->canvasX + nodeWidth(*playerNode) + 40.0f
          + static_cast<float>(chain.size()) * 250.0f
        : 0.0f;

    node->canvasX = column;
    node->canvasY = row;

    const NodeId added = graph.addNode(std::move(node));
    if (added == kInvalidNode) return kInvalidNode;

    if (!loaded) {
        if (Node* placed = graph.node(added)) placed->setErrorText(error);
    }

    for (const Connection& c : downstream) graph.disconnect(c.id);

    graph.connect(tailNode, tailPort, added, 0);
    for (const Connection& c : downstream) graph.connect(added, 0, c.destNode, c.destPort);

    selection_.clear();
    selection_.push_back(added);
    return added;
}

bool PatcherView::removeFromChain(NodeId nodeId) {
    if (!engine_) return false;

    Graph& graph = engine_->graph();
    const Node* node = graph.node(nodeId);
    if (!node) return false;

    // Remember what fed it and what it fed, so the gap can be closed.
    NodeId upstreamNode = kInvalidNode;
    PortIndex upstreamPort = 0;
    std::vector<Connection> downstream;

    for (const Connection& c : graph.connections()) {
        if (c.destNode == nodeId && c.destPort == 0) {
            upstreamNode = c.sourceNode;
            upstreamPort = c.sourcePort;
        }
        if (c.sourceNode == nodeId && c.sourcePort == 0) downstream.push_back(c);
    }

    // A plugin's editor has to go before the plugin does.
    if (auto* plugin = dynamic_cast<vst2::VstNode*>(graph.node(nodeId))) plugin->closeEditor();

    if (!graph.removeNode(nodeId)) return false;

    if (upstreamNode != kInvalidNode) {
        for (const Connection& c : downstream)
            graph.connect(upstreamNode, upstreamPort, c.destNode, c.destPort);
    }

    selection_.erase(std::remove(selection_.begin(), selection_.end(), nodeId), selection_.end());
    return true;
}

int PatcherView::copyStemChain(NodeId stemPlayer, int fromSlot, int toSlot) {
    if (!engine_ || fromSlot == toSlot) return 0;

    Graph& graph = engine_->graph();
    const std::vector<NodeId> source =
        downstreamChain(graph, stemPlayer, static_cast<PortIndex>(fromSlot));

    int copied = 0;
    for (NodeId id : source) {
        auto* plugin = dynamic_cast<vst2::VstNode*>(graph.node(id));
        if (!plugin) continue;   // only plugins are worth duplicating

        const NodeId added = addToStemChain(stemPlayer, toSlot, plugin->pluginDescription(),
                                            plugin->bridged());
        if (added == kInvalidNode) continue;

        // Carry the settings across, not just the plugin. Copying a rack that
        // arrives at its defaults would mean dialling every one of them in
        // again, which is most of the work the copy was meant to save.
        if (auto* clone = dynamic_cast<vst2::VstNode*>(graph.node(added))) {
            if (plugin->pluginLoaded() && clone->pluginLoaded()) {
                if (vst2::Vst2Plugin* from = plugin->plugin()) {
                    if (vst2::Vst2Plugin* to = clone->plugin()) {
                        const std::vector<std::uint8_t> state = from->saveState();
                        if (!state.empty()) to->restoreState(state);
                        else {
                            for (int p = 0; p < std::min(from->parameterCount(),
                                                         to->parameterCount()); ++p)
                                to->setParameterValue(p, from->parameterValue(p));
                        }
                        clone->refreshParametersFromPlugin();
                    }
                }
            }
            clone->setName(plugin->name());
        }
        ++copied;
    }

    return copied;
}

library::ChainPreset PatcherView::captureStemChain(NodeId stemPlayer, int stemSlot,
                                                  std::string name) const {
    library::ChainPreset preset;
    preset.name = std::move(name);
    if (!engine_) return preset;

    const Graph& graph = engine_->graph();
    for (NodeId id : downstreamChain(graph, stemPlayer, static_cast<PortIndex>(stemSlot))) {
        const auto* plugin = dynamic_cast<const vst2::VstNode*>(graph.node(id));
        if (!plugin) continue;

        library::ChainPlugin entry;
        const vst2::PluginDescription& description = plugin->pluginDescription();
        entry.path = description.path;
        entry.name = description.name;
        entry.uniqueId = description.uniqueId;
        entry.bridged = plugin->bridged();

        // The chunk where the plugin offers one, the parameter list otherwise.
        // Storing both would double the size of the file for nothing: a plugin
        // that uses chunks ignores the parameters on the way back in.
        if (vst2::Vst2Plugin* live = const_cast<vst2::VstNode*>(plugin)->plugin()) {
            entry.state = live->saveState();
            if (entry.state.empty()) {
                entry.parameters.reserve(static_cast<std::size_t>(live->parameterCount()));
                for (int p = 0; p < live->parameterCount(); ++p)
                    entry.parameters.push_back(live->parameterValue(p));
            }
        }

        preset.plugins.push_back(std::move(entry));
    }

    return preset;
}

int PatcherView::applyStemChain(NodeId stemPlayer, int stemSlot,
                                const library::ChainPreset& preset,
                                std::vector<std::string>* outMissing) {
    if (!engine_ || !plugins_) return 0;

    Graph& graph = engine_->graph();
    if (const Node* player = graph.node(stemPlayer)) {
        if (stemSlot < 0 || stemSlot >= player->numOutputs()) return 0;
    } else {
        return 0;
    }

    // Tear the old rack down first, back to front so each removal closes its own
    // gap rather than leaving the next one dangling.
    const std::vector<NodeId> existing =
        downstreamChain(graph, stemPlayer, static_cast<PortIndex>(stemSlot));
    for (auto it = existing.rbegin(); it != existing.rend(); ++it)
        if (dynamic_cast<vst2::VstNode*>(graph.node(*it))) removeFromChain(*it);

    int placed = 0;
    for (const library::ChainPlugin& entry : preset.plugins) {
        // Path first, unique id second. A library carried to another machine
        // keeps its ids and loses its paths, and that is the case worth
        // surviving; the reverse - the same path holding a different plugin -
        // is the case worth refusing.
        const vst2::PluginDescription* description = nullptr;
        for (const vst2::PluginDescription& candidate : plugins_->plugins()) {
            if (candidate.path == entry.path) { description = &candidate; break; }
        }
        if (!description && entry.uniqueId != 0) {
            for (const vst2::PluginDescription& candidate : plugins_->plugins()) {
                if (candidate.uniqueId == entry.uniqueId) { description = &candidate; break; }
            }
        }

        if (!description) {
            if (outMissing) outMissing->push_back(entry.name.empty() ? entry.path : entry.name);
            continue;
        }

        const NodeId added = addToStemChain(stemPlayer, stemSlot, *description, entry.bridged);
        if (added == kInvalidNode) {
            if (outMissing) outMissing->push_back(entry.name);
            continue;
        }

        if (auto* node = dynamic_cast<vst2::VstNode*>(graph.node(added))) {
            if (vst2::Vst2Plugin* live = node->plugin()) {
                if (!entry.state.empty()) live->restoreState(entry.state);
                else {
                    const int count = std::min(static_cast<int>(entry.parameters.size()),
                                               live->parameterCount());
                    for (int p = 0; p < count; ++p)
                        live->setParameterValue(p, entry.parameters[static_cast<std::size_t>(p)]);
                }
                node->refreshParametersFromPlugin();
            }
        }
        ++placed;
    }

    tidyStemChains(stemPlayer);
    return placed;
}

void PatcherView::tidyStemChains(NodeId stemPlayer) {
    if (!engine_) return;

    Graph& graph = engine_->graph();
    const Node* player = graph.node(stemPlayer);
    if (!player) return;

    const float left = player->canvasX + nodeWidth(*player) + 40.0f;

    for (int slot = 0; slot < player->numOutputs(); ++slot) {
        const std::vector<NodeId> chain =
            downstreamChain(graph, stemPlayer, static_cast<PortIndex>(slot));

        for (std::size_t i = 0; i < chain.size(); ++i) {
            if (Node* node = graph.node(chain[i])) {
                node->canvasX = left + static_cast<float>(i) * 250.0f;
                node->canvasY = player->canvasY + static_cast<float>(slot) * 130.0f;
            }
        }
    }
}

bool PatcherView::handleFileDrop(const std::string& utf8Path, Vec2 screenPosition,
                                 const Rect& bounds) {
    const float z = viewScale();
    if (!engine_ || !audiofile::isSupportedFile(utf8Path)) return false;

    // Dropping onto an existing sample player replaces its file; dropping on
    // empty canvas makes a new one. Both are what people try first.
    const Vec2 world = screenToWorld(screenPosition, bounds);

    // Whichever node is on top there, by the same rule the pointer uses.
    //
    // This used to be several races instead of one decision. Each node body
    // called acceptDrop for its own strip while it drew, and acceptDrop tests a
    // bare rectangle - no clip stack, no z-order - so the *first node drawn*
    // whose rectangle covered the pointer won, even when another node was drawn
    // over it and was the only one visible there.
    const NodeId target = topmostNodeAt(screenPosition, bounds);
    if (Node* node = target != kInvalidNode ? engine_->graph().node(target) : nullptr) {
        const Rect box = nodeBounds(*node, bounds);

        if (node->typeName() == "sample.player") {
            auto* player = static_cast<SamplePlayerNode*>(node);
            return player->loadFile(utf8Path, nullptr);
        }

        // Which of the three layers, from where in the node it landed. Above
        // them - on the fire button - fills the first empty one, which is what
        // three drags in a row should do.
        if (node->typeName() == "drop") {
            auto* drop = static_cast<DropNode*>(node);
            const float scale = z;
            const float bodyTop = box.top() + kHeaderHeight * scale;
            const float layersTop = bodyTop + dropBody::layersOffset() * scale;
            const float layerHeight = dropBody::kLayerHeight * scale;

            int layer = 0;
            if (screenPosition.y > layersTop && layerHeight > 0.0f) {
                layer = static_cast<int>((screenPosition.y - layersTop) / layerHeight);
            } else {
                for (int i = 0; i < DropNode::kLayers; ++i)
                    if (!drop->layerSample(i)) { layer = i; break; }
            }

            return drop->loadLayer(clampValue(layer, 0, DropNode::kLayers - 1),
                                   utf8Path, nullptr);
        }

        // A stem player has eight slots stacked down its body, so which one was
        // hit has to come from where in the node the pointer landed. The strips
        // are the bottom of the body, laid out by drawStemPlayerBody.
        if (node->typeName() == "stem.player") {
            auto* player = static_cast<StemPlayerNode*>(node);

            // The strips' position comes from the same constants that lay them
            // out, rather than a second copy of the arithmetic. The header is
            // kHeaderHeight in design units for the same reason everything else
            // here is: the theme's metrics already carry the display scale, and
            // the canvas multiplies by viewScale() on the way out.
            const float scale = z;
            const float bodyTop = box.top() + kHeaderHeight * scale;
            const float stripsTop = bodyTop + stemBody::stripsOffset() * scale;
            const float stripHeight = stemBody::kStripHeight * scale;

            int slot = 0;
            if (screenPosition.y > stripsTop && stripHeight > 0.0f)
                slot = static_cast<int>((screenPosition.y - stripsTop) / stripHeight);

            // Dropping anywhere above the strips fills the first free slot,
            // which is what happens when several files are dragged in at once.
            if (screenPosition.y <= stripsTop) {
                slot = 0;
                for (int i = 0; i < kMaxStems; ++i) {
                    if (!player->stemLoaded(i)) { slot = i; break; }
                }
            }

            return player->loadStem(clampValue(slot, 0, kMaxStems - 1), utf8Path, nullptr);
        }

        if (node->typeName() == "build") {
            auto* build = static_cast<BuildNode*>(node);
            return build->loadRiser(utf8Path, nullptr);
        }
    }

    auto player = std::make_unique<SamplePlayerNode>();
    std::string error;
    player->loadFile(utf8Path, &error);

    return placeNode(std::move(player), world) != kInvalidNode;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void PatcherView::drawGrid(Ui& ui, const Rect& bounds) const {
    const float z = viewScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(bounds, t.canvas);

    const float spacing = kGridSpacing * z;
    if (spacing < 6.0f) return;   // too dense to be anything but noise

    // Every fourth line is brighter, which gives the eye something to measure
    // distance against when the canvas is otherwise empty.
    const float originX = std::fmod(pan_.x * z, spacing * 4.0f);
    const float originY = std::fmod(pan_.y * z, spacing * 4.0f);

    int index = 0;
    for (float x = bounds.left() + originX - spacing * 4.0f; x < bounds.right(); x += spacing, ++index) {
        if (x < bounds.left()) continue;
        const bool major = (index % 4) == 0;
        list.addRectFilled(Rect{ std::floor(x), bounds.top(), 1.0f, bounds.height },
                           major ? t.canvasGridMajor : t.canvasGrid);
    }

    index = 0;
    for (float y = bounds.top() + originY - spacing * 4.0f; y < bounds.bottom(); y += spacing, ++index) {
        if (y < bounds.top()) continue;
        const bool major = (index % 4) == 0;
        list.addRectFilled(Rect{ bounds.left(), std::floor(y), bounds.width, 1.0f },
                           major ? t.canvasGridMajor : t.canvasGrid);
    }
}

void PatcherView::drawCables(Ui& ui, const Rect& bounds) {
    const float z = viewScale();
    if (!engine_) return;

    const Theme& t = theme();
    DrawList& list = ui.draw();
    const Graph& graph = engine_->graph();

    for (const Connection& connection : graph.connections()) {
        const Node* source = graph.node(connection.sourceNode);
        const Node* destination = graph.node(connection.destNode);
        if (!source || !destination) continue;

        const Vec2 from = portPosition(*source, connection.sourcePort, false, bounds);
        const Vec2 to = portPosition(*destination, connection.destPort, true, bounds);

        // Skip cables entirely outside the view.
        const Rect span = Rect::fromCorners(from, to).inflated(80.0f);
        if (!span.intersects(bounds)) continue;

        const bool feedback = graph.isFeedbackEdge(connection.id);
        const bool hovered = hoveredCable_ == connection.id;
        const bool endpointSelected = isSelected(connection.sourceNode)
                                   || isSelected(connection.destNode);

        Colour colour = feedback ? t.cableFeedback : t.categoryColour(source->category());
        float thickness = kCableThickness * z;

        if (hovered) { colour = t.cableHover; thickness *= 1.6f; }
        else if (endpointSelected) colour = colour.brightened(1.3f);
        else colour = colour.withAlpha(0.55f);

        const float tension = cableTension(from, to);
        const Vec2 control1{ from.x + tension, from.y };
        const Vec2 control2{ to.x - tension, to.y };

        // A dark under-stroke keeps cables legible where they cross each other.
        list.addBezier(from, control1, control2, to, Colour{ 0.0f, 0.0f, 0.0f, 0.55f },
                       thickness + 2.5f);
        list.addBezier(from, control1, control2, to, colour, thickness);

        if (feedback) {
            // Feedback edges are worth calling out: they cost a block of latency
            // and the performer should know where they are.
            const Vec2 midpoint{ (from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f };
            list.addCircleFilled(midpoint, 4.0f * z, t.canvas);
            list.addCircle(midpoint, 4.0f * z, t.cableFeedback, 1.5f);
        }
    }

    // The cable currently being dragged.
    if (mode_ == CanvasMode::DraggingCable && cableSource_.valid()) {
        const Node* node = graph.node(cableSource_.node);
        if (node) {
            const Vec2 from = portPosition(*node, cableSource_.port, cableSource_.isInput, bounds);
            const Vec2 to = ui.input().mousePosition;
            const float tension = cableTension(from, to);

            const Vec2 control1 = cableSource_.isInput ? Vec2{ from.x - tension, from.y }
                                                       : Vec2{ from.x + tension, from.y };
            const Vec2 control2 = cableSource_.isInput ? Vec2{ to.x + tension, to.y }
                                                       : Vec2{ to.x - tension, to.y };

            list.addBezier(from, control1, control2, to, t.accent.withAlpha(0.9f),
                           kCableThickness * z * 1.2f);
            list.addCircleFilled(to, 4.0f, t.accent);
        }
    }
}

void PatcherView::drawNode(Ui& ui, Node& node, const Rect& bounds, bool interactive) {
    // Everything below draws the same either way; only the claiming changes.
    if (!interactive) ui.pushInert();
    struct InertGuard {
        Ui& ui; bool on;
        ~InertGuard() { if (!on) ui.popInert(); }
    } guard{ ui, interactive };

    const float z = viewScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();

    const Rect rect = nodeBounds(node, bounds);
    if (!rect.intersects(bounds)) return;   // fully scrolled off

    const bool selected = isSelected(node.id());
    const bool hovered = hovered_ == node.id();
    const Colour accent = t.categoryColour(node.category());
    const bool bypassed = node.bypassed();

    // Drop shadow, then body, then a coloured header.
    list.addRectFilled(rect.translated({ 0.0f, 3.0f }), Colour{ 0.0f, 0.0f, 0.0f, 0.45f },
                       t.cornerRadiusLarge);

    if (selected)
        list.addGlow(rect, accent.withAlpha(0.55f), 10.0f, t.cornerRadiusLarge, 5);

    list.addRectFilled(rect, bypassed ? t.panel.withAlpha(0.7f) : t.panelRaised, t.cornerRadiusLarge);

    const Rect header{ rect.left(), rect.top(), rect.width, kHeaderHeight * z };
    list.addRectFilledGradient(header, accent.withAlpha(bypassed ? 0.12f : 0.30f),
                               accent.withAlpha(bypassed ? 0.04f : 0.10f),
                               t.cornerRadiusLarge, gfx::Corners::Top);
    // A solid stripe down the left edge is the fastest category read at a glance.
    list.addRectFilled(Rect{ rect.left(), rect.top(), 3.0f * z, rect.height },
                       accent.withAlpha(bypassed ? 0.3f : 0.9f),
                       t.cornerRadiusLarge, gfx::Corners::Left);

    list.addRect(rect, selected ? accent : (hovered ? t.borderStrong : t.border),
                 selected ? 1.6f : t.borderWidth, t.cornerRadiusLarge);

    // -- header content ----------------------------------------------------
    Rect headerContent = header.deflated(6.0f * z);
    headerContent.removeFromLeft(4.0f * z);

    const Rect collapseArea = headerContent.removeFromRight(16.0f * z);
    const Rect bypassArea = headerContent.removeFromRight(18.0f * z);

    if (z > 0.55f) {
        if (ui.iconButton(ui.idFrom(&node, 900), collapseArea, Ui::Icon::Chevron,
                          node.collapsed ? t.textDim : t.textFaint))
            node.collapsed = !node.collapsed;

        if (ui.iconButton(ui.idFrom(&node, 901), bypassArea, Ui::Icon::Power,
                          bypassed ? t.danger : t.textFaint, bypassed))
            node.setBypassed(!bypassed);
        if (ui.isHot(ui.idFrom(&node, 901)))
            ui.setTooltip(bypassed ? "Bypassed - audio passes straight through"
                                   : "Bypass this node");
    }

    // Double-clicking the name renames it in place.
    const UiId renameId = ui.idFrom(&node, 902);
    if (renamingNode_ == node.id()) {
        if (ui.textField(renameId, headerContent, renameBuffer_)) {
            if (!renameBuffer_.empty()) node.setName(renameBuffer_);
            renamingNode_ = kInvalidNode;
        }
    } else {
        list.addTextClipped(ui.font(t.fontUiBold), headerContent,
                            bypassed ? t.textFaint : t.text, node.name());

        if (ui.hovering(headerContent)
            && ui.input().mouseDoubleClicked[static_cast<int>(MouseButton::Left)]) {
            renamingNode_ = node.id();
            renameBuffer_ = node.name();
            ui.beginTextEdit(renameId, renameBuffer_, true);
        }
    }

    if (node.collapsed) return;

    // -- body --------------------------------------------------------------
    Rect body{ rect.left() + 6.0f * z, header.bottom() + 2.0f * z,
               rect.width - 12.0f * z,
               rect.bottom() - header.bottom() - 6.0f * z };

    if (!node.errorText().empty()) {
        const Rect errorRow = body.removeFromBottom(16.0f * z);
        list.addRectFilled(errorRow, t.danger.withAlpha(0.12f), 2.0f);
        list.addTextClipped(ui.font(t.fontSmall), errorRow.deflated(3.0f), t.danger,
                            node.errorText());
        if (ui.hovering(errorRow)) ui.setTooltip(node.errorText());
    }

    // Below this the controls are too small to hit reliably, so the node shows
    // only its identity and its meters.
    if (z > 0.62f && body.height > 12.0f) {
        // Clipped to the node. Body layouts are authored in unscaled units and
        // scaled by bodyScale(); rounding and text that cannot shrink below its
        // atlas size mean the last row can still overhang by a pixel or two, and
        // a control drawn outside its own node is one that can be clicked
        // through another node sitting on top of it.
        ui.pushClip(body);
        drawNodeBody(ui, node, body);
        ui.popClip();
    }

    // -- ports -------------------------------------------------------------
    for (int p = 0; p < node.numInputs(); ++p) {
        const Vec2 position = portPosition(node, p, true, bounds);
        const bool connected = [&] {
            for (const Connection& c : engine_->graph().connections())
                if (c.destNode == node.id() && c.destPort == p) return true;
            return false;
        }();

        const float radius = kPortRadius * z;
        list.addCircleFilled(position, radius, connected ? accent : t.panelSunken);
        list.addCircle(position, radius, connected ? accent.brightened(1.2f) : t.borderStrong, 1.4f);

        if (ui.hovering(Rect{ position.x - radius * 2.0f, position.y - radius * 2.0f,
                              radius * 4.0f, radius * 4.0f })) {
            list.addCircle(position, radius + 3.0f, t.accent, 1.5f);
            ui.setTooltip(node.name() + " in: " + node.inputPort(p).name);
        }
    }

    for (int p = 0; p < node.numOutputs(); ++p) {
        const Vec2 position = portPosition(node, p, false, bounds);
        const bool connected = [&] {
            for (const Connection& c : engine_->graph().connections())
                if (c.sourceNode == node.id() && c.sourcePort == p) return true;
            return false;
        }();

        const float radius = kPortRadius * z;
        list.addCircleFilled(position, radius, connected ? accent : t.panelSunken);
        list.addCircle(position, radius, connected ? accent.brightened(1.2f) : t.borderStrong, 1.4f);

        if (ui.hovering(Rect{ position.x - radius * 2.0f, position.y - radius * 2.0f,
                              radius * 4.0f, radius * 4.0f })) {
            list.addCircle(position, radius + 3.0f, t.accent, 1.5f);
            ui.setTooltip(node.name() + " out: " + node.outputPort(p).name);
        }
    }

    // Resize grip on the right edge.
    const Rect grip{ rect.right() - kResizeHandleWidth, rect.top() + rect.height * 0.5f - 12.0f,
                     kResizeHandleWidth, 24.0f };
    if (ui.hovering(grip)) {
        ui.setCursor(Cursor::ResizeHorizontal);
        list.addRectFilled(grip.deflated(2.0f), t.borderStrong.withAlpha(0.6f), 1.5f);

        if (ui.input().mousePressed[static_cast<int>(MouseButton::Left)] && !ui.pointerCaptured()) {
            mode_ = CanvasMode::ResizingNode;
            resizingNode_ = node.id();
            resizeStartWidth_ = nodeWidth(node);
            dragStartWorld_ = screenToWorld(ui.input().mousePosition, bounds);
        }
    }
}

// ---------------------------------------------------------------------------
// Node bodies
// ---------------------------------------------------------------------------

void PatcherView::drawNodeBody(Ui& ui, Node& node, const Rect& body) {
    const std::string& type = node.typeName();

    if (type == "sample.player") { drawSamplePlayerBody(ui, node, body); return; }
    if (type == "stem.player") { drawStemPlayerBody(ui, node, body); return; }
    if (type == "color") { drawColorBody(ui, node, body); return; }
    if (type == "build") { drawBuildBody(ui, node, body); return; }
    if (type == "drop") { drawDropBody(ui, node, body); return; }
    if (type == "looper") { drawLooperBody(ui, node, body); return; }
    if (type == "crossfader") { drawCrossfaderBody(ui, node, body); return; }
    if (type.rfind("mixer.", 0) == 0) { drawMixerBody(ui, node, body); return; }
    if (type == vst2::VstNode::kTypeName) { drawPluginBody(ui, node, body); return; }

    drawGenericBody(ui, node, body);
}

void PatcherView::drawSamplePlayerBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& player = static_cast<SamplePlayerNode&>(node);

    Rect area = body;

    // -- waveform strip ----------------------------------------------------
    const Rect waveform = area.removeFromTop(area.height * 0.42f);
    list.addRectFilled(waveform, t.panelSunken, 2.0f);

    const auto sample = player.sample();
    if (sample && !sample->overview().minimum.empty()) {
        const auto& overview = sample->overview();
        const float centreY = waveform.centre().y;
        const float halfHeight = waveform.height * 0.45f;

        const int columns = std::max(1, static_cast<int>(waveform.width));
        for (int x = 0; x < columns; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(columns);
            const auto bucket = static_cast<std::size_t>(u * static_cast<float>(overview.buckets - 1));
            const float low = overview.minimum[bucket];
            const float high = overview.maximum[bucket];

            const float y0 = centreY - high * halfHeight;
            const float y1 = centreY - low * halfHeight;
            list.addRectFilled(Rect{ waveform.left() + static_cast<float>(x), y0,
                                     1.0f, std::max(1.0f, y1 - y0) },
                               t.accent.withAlpha(0.75f));
        }

        // Loop region and playhead.
        const float start = player.parameter(node.indexOfParameter("start")).value();
        const float end = player.parameter(node.indexOfParameter("end")).value();
        const Rect loopRegion{ waveform.left() + waveform.width * start, waveform.top(),
                               waveform.width * std::max(0.0f, end - start), waveform.height };
        list.addRectFilled(loopRegion, t.control.withAlpha(0.10f));
        list.addRectFilled(Rect{ loopRegion.left(), waveform.top(), 1.0f, waveform.height },
                           t.control.withAlpha(0.8f));
        list.addRectFilled(Rect{ loopRegion.right() - 1.0f, waveform.top(), 1.0f, waveform.height },
                           t.control.withAlpha(0.8f));

        if (player.sounding()) {
            const float playheadX = waveform.left() + waveform.width * player.playPositionNormalised();
            list.addRectFilled(Rect{ playheadX, waveform.top(), 1.5f, waveform.height }, t.text);
        }
    } else {
        list.addTextClipped(ui.font(t.fontSmall), waveform, t.textFaint,
                            "drop an audio file here", DrawList::Align::Centre);
    }

    area.removeFromTop(s * 4.0f);

    // -- transport row -----------------------------------------------------
    Rect controls = area.removeFromTop(s * 20.0f);
    const Rect playButton = controls.removeFromLeft(s * 26.0f);
    controls.removeFromLeft(s * 4.0f);

    Parameter& play = node.parameter(node.indexOfParameter("play"));
    const bool playing = play.boolValue();
    if (ui.iconButton(ui.idFrom(&node, 10), playButton,
                      playing ? Ui::Icon::Stop : Ui::Icon::Play,
                      playing ? t.accent : t.textDim, playing))
        play.setValue(playing ? 0.0f : 1.0f);

    const Rect syncCombo = controls.removeFromLeft(std::min(72.0f, controls.width * 0.5f));
    ui.parameterChoice(syncCombo, node.parameter(node.indexOfParameter("sync")));
    controls.removeFromLeft(s * 4.0f);

    ui.parameterSlider(controls, node.parameter(node.indexOfParameter("loopbeats")), t.control, false);

    area.removeFromTop(s * 3.0f);

    // -- level row ---------------------------------------------------------
    Rect levels = area.removeFromTop(std::min(20.0f, area.height));
    const Rect meterArea = levels.removeFromRight(s * 14.0f);
    levels.removeFromRight(s * 4.0f);

    ui.parameterSlider(levels, node.parameter(node.indexOfParameter("gain")), t.accent, false);
    ui.stereoMeter(meterArea, player.meterLevel(0), player.meterLevel(1), true);
}

void PatcherView::drawLooperBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& looper = static_cast<LooperNode&>(node);

    Rect area = body;

    // -- take overview -----------------------------------------------------
    const Rect strip = area.removeFromTop(area.height * 0.36f);
    list.addRectFilled(strip, t.panelSunken, 2.0f);

    const auto& overview = looper.overview();
    if (!overview.empty() && looper.loopFrames() > 0) {
        const float centreY = strip.centre().y;
        const float halfHeight = strip.height * 0.45f;
        const int columns = std::max(1, static_cast<int>(strip.width));

        for (int x = 0; x < columns; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(columns);
            const auto bucket = static_cast<std::size_t>(u * static_cast<float>(overview.size() - 1));
            const float peak = overview[bucket] * halfHeight;
            list.addRectFilled(Rect{ strip.left() + static_cast<float>(x), centreY - peak,
                                     1.0f, std::max(1.0f, peak * 2.0f) },
                               t.categoryColour(node.category()).withAlpha(0.75f));
        }
    }

    const LooperNode::State state = looper.state();
    if (state != LooperNode::State::Empty && looper.loopFrames() > 0) {
        const float x = strip.left() + strip.width * looper.positionNormalised();
        list.addRectFilled(Rect{ x, strip.top(), 1.5f, strip.height },
                           state == LooperNode::State::Recording ? t.recording : t.text);
    }

    // A recording looper pulses, so it is unmistakable across a room.
    if (state == LooperNode::State::Recording || state == LooperNode::State::Overdubbing) {
        static float pulse = 0.0f;
        pulse += ui.deltaSeconds() * 3.2f;
        const float alpha = 0.25f + 0.25f * (0.5f + 0.5f * std::sin(pulse));
        list.addRect(strip, t.recording.withAlpha(alpha), 1.5f, 2.0f);
    }

    Rect statusRow = area.removeFromTop(s * 14.0f);
    char status[96];
    std::snprintf(status, sizeof(status), "%s  %.2fs", looper.stateName(), looper.loopSeconds());
    list.addTextClipped(ui.font(t.fontSmall), statusRow,
                        state == LooperNode::State::Recording ? t.recording : t.textDim, status);

    area.removeFromTop(s * 2.0f);

    // -- transport ---------------------------------------------------------
    Rect controls = area.removeFromTop(s * 20.0f);
    const float buttonWidth = std::min(30.0f, controls.width / 4.5f);

    if (ui.iconButton(ui.idFrom(&node, 20), controls.removeFromLeft(buttonWidth),
                      Ui::Icon::Record, t.recording,
                      state == LooperNode::State::Recording))
        looper.toggleRecord();
    controls.removeFromLeft(s * 3.0f);

    if (ui.iconButton(ui.idFrom(&node, 21), controls.removeFromLeft(buttonWidth),
                      Ui::Icon::Play, t.accent,
                      state == LooperNode::State::Playing || state == LooperNode::State::Overdubbing))
        looper.togglePlay();
    controls.removeFromLeft(s * 3.0f);

    if (ui.iconButton(ui.idFrom(&node, 22), controls.removeFromLeft(buttonWidth),
                      Ui::Icon::Plus, t.control, state == LooperNode::State::Overdubbing))
        looper.toggleOverdub();
    if (ui.isHot(ui.idFrom(&node, 22))) ui.setTooltip("Overdub onto the take");
    controls.removeFromLeft(s * 3.0f);

    if (ui.iconButton(ui.idFrom(&node, 23), controls.removeFromLeft(buttonWidth),
                      Ui::Icon::Trash, t.textDim))
        looper.clear();

    area.removeFromTop(s * 3.0f);

    Rect levels = area.removeFromTop(std::min(20.0f, area.height));
    const Rect meterArea = levels.removeFromRight(s * 14.0f);
    levels.removeFromRight(s * 4.0f);
    ui.parameterSlider(levels, node.parameter(node.indexOfParameter("feedback")), t.control, false);
    ui.stereoMeter(meterArea, looper.meterLevel(0), looper.meterLevel(1), true);
}

void PatcherView::drawCrossfaderBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& fader = static_cast<CrossfaderNode&>(node);

    Rect area = body;

    // A and B labels with their live gains, so the fader's effect is visible
    // even when the audio is not obviously changing.
    Rect labels = area.removeFromTop(s * 13.0f);
    const Rect labelA = labels.removeFromLeft(labels.width * 0.5f);
    list.addTextClipped(ui.font(t.fontSmall), labelA,
                        fader.sideGain(0) > 0.5f ? t.accent : t.textFaint, "A");
    list.addTextClipped(ui.font(t.fontSmall), labels,
                        fader.sideGain(1) > 0.5f ? t.accent : t.textFaint, "B",
                        DrawList::Align::Right);

    // The fader itself gets the most vertical space of anything on the node.
    Rect faderArea = area.removeFromTop(s * 24.0f);
    Parameter& position = node.parameter(fader.positionParam());
    float normalised = position.normalised();

    if (ui.sliderNormalised(ui.idFrom(&node, 30), faderArea, normalised, t.control))
        position.setNormalised(normalised);

    // Centre detent marker.
    list.addRectFilled(Rect{ faderArea.centre().x - 0.5f, faderArea.top() - 2.0f, 1.0f, 3.0f },
                       t.textFaint);

    area.removeFromTop(s * 4.0f);

    Rect cuts = area.removeFromTop(s * 18.0f);
    const Rect cutA = cuts.removeFromLeft(cuts.width * 0.5f - 2.0f);
    cuts.removeFromLeft(s * 4.0f);
    ui.parameterToggle(cutA, node.parameter(node.indexOfParameter("cuta")), t.danger);
    ui.parameterToggle(cuts, node.parameter(node.indexOfParameter("cutb")), t.danger);

    area.removeFromTop(s * 3.0f);
    if (area.height >= s * 16.0f) {
        Rect curveRow = area.removeFromTop(s * 18.0f);
        ui.parameterChoice(curveRow, node.parameter(node.indexOfParameter("curve")));
    }
}


namespace {

// A stable colour per section. Taken from the section's own hue when it has one
// so the marker on the spectral strip and the cell on the grid are the same
// colour, and spread around the wheel by index otherwise.
gfx::Colour sectionColour(const StemSection& section, int index) {
    const float hue = section.hue > 0.0f ? section.hue
                                         : std::fmod(static_cast<float>(index) * 0.137f, 1.0f);

    // Six-segment wheel, kept desaturated enough to read against the dark lab
    // palette rather than shouting over the spectrum underneath.
    const float h = hue * 6.0f;
    const int segment = static_cast<int>(h) % 6;
    const float f = h - std::floor(h);

    const float high = 0.85f, low = 0.35f;
    const float rise = low + (high - low) * f;
    const float fall = high - (high - low) * f;

    switch (segment) {
        case 0:  return { high, rise, low, 1.0f };
        case 1:  return { fall, high, low, 1.0f };
        case 2:  return { low, high, rise, 1.0f };
        case 3:  return { low, fall, high, 1.0f };
        case 4:  return { rise, low, high, 1.0f };
        default: return { high, low, fall, 1.0f };
    }
}

} // namespace

void PatcherView::drawStemPlayerBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& stems = static_cast<StemPlayerNode&>(node);

    Rect area = body;

    // -- sections ----------------------------------------------------------
    // The grid is the instrument. It is drawn first and given the most room,
    // because during a set this is the only part of the node anyone touches.
    const int count = stems.sectionCount();
    const int active = stems.activeSection();
    const int pending = stems.pendingSection();

    Rect sectionArea = area.removeFromTop(s * stemBody::kSections);
    list.addRectFilled(sectionArea, t.canvas.brightened(1.08f), t.cornerRadius);

    if (count == 0) {
        list.addTextClipped(ui.font(t.fontSmall), sectionArea, t.textFaint,
                            "no sections - add them in the inspector",
                            DrawList::Align::Centre);
    } else {
        // Four to a row, which fits a sixteen-section song in the node without
        // scrolling and stays big enough to hit on a touchscreen.
        const int columns = 4;
        const int rows = (count + columns - 1) / columns;
        Rect grid = sectionArea.deflated(s * 4.0f);
        const float cellHeight = grid.height / static_cast<float>(std::max(1, rows));

        for (int row = 0; row < rows; ++row) {
            Rect rowRect = grid.removeFromTop(cellHeight);
            const int first = row * columns;
            const int inRow = std::min(columns, count - first);
            const float cellWidth = rowRect.width / static_cast<float>(columns);

            for (int column = 0; column < inRow; ++column) {
                const int index = first + column;
                Rect cell = rowRect.removeFromLeft(cellWidth).deflated(s * 2.0f);

                const StemSection& section = stems.sections()[static_cast<std::size_t>(index)];
                const bool isActive = index == active;
                const bool isPending = index == pending;

                const Colour marker = sectionColour(section, index);
                Colour fill = t.widgetBackground;
                if (isActive) fill = marker.withAlpha(0.34f);
                else if (isPending) fill = marker.withAlpha(0.20f);

                bool hovered = false, held = false;
                const UiId cellId = ui.idFrom(&node, 400 + index);
                if (ui.buttonBehaviour(cellId, cell, hovered, held))
                    stems.requestSection(index);
                if (hovered) fill = fill.brightened(1.4f);

                list.addRectFilled(cell, fill, t.cornerRadius);

                // The active cell carries a progress bar along its bottom edge,
                // so how long is left of the loop is readable without looking
                // anywhere else.
                if (isActive) {
                    const float progress = stems.loopProgress();
                    list.addRectFilled(Rect{ cell.left(), cell.bottom() - 3.0f,
                                             cell.width * progress, 3.0f }, marker);
                    list.addRect(cell, marker, 1.0f, t.cornerRadius);
                } else if (isPending) {
                    list.addRect(cell, marker.withAlpha(0.8f), 1.0f, t.cornerRadius);
                }

                list.addTextClipped(ui.font(t.fontSmall), cell.deflated(s * 3.0f),
                                    isActive ? t.text : t.textDim, section.name,
                                    DrawList::Align::Centre);
            }
        }
    }

    area.removeFromTop(s * stemBody::kAfterSections);

    // -- launch and divide -------------------------------------------------
    Rect controlRow = area.removeFromTop(s * stemBody::kControlRow);
    ui.parameterCycle(controlRow.removeFromLeft(controlRow.width * 0.55f),
                      node.parameter(node.indexOfParameter("launch")), t.accent);
    controlRow.removeFromLeft(s * 4.0f);
    ui.parameterCycle(controlRow, node.parameter(node.indexOfParameter("divide")), t.control);

    area.removeFromTop(s * stemBody::kAfterControls);

    // -- tempo -------------------------------------------------------------
    // 20 rather than 18: a button's label needs its font height plus the
    // padding the style puts around it, and at 18 the toggle lost the tail of
    // its g.
    Rect tempoRow = area.removeFromTop(s * stemBody::kTempoRow);

    // The routing toggle *leads* the row. Trailing it put it at the extreme
    // right edge of the widest node in the application, which on any ordinary
    // window sits underneath the inspector - drawn behind it and clipped out of
    // the canvas, so it was invisible and could not be pressed. That is exactly
    // how it behaved, and no amount of fixing the overlap with its neighbour was
    // ever going to help, because the neighbour was not the problem.
    const Rect matrixToggle = tempoRow.removeFromLeft(s * 74.0f);
    tempoRow.removeFromLeft(s * 4.0f);

    if (ui.button(ui.idFrom(&node, 422), matrixToggle,
                  stems.matrixOpen ? "routing <" : "routing >",
                  stems.matrixOpen ? Ui::ButtonStyle::Toggle : Ui::ButtonStyle::Normal,
                  stems.matrixOpen))
        stems.matrixOpen = !stems.matrixOpen;
    if (ui.isHot(ui.idFrom(&node, 422)))
        ui.setTooltip("Show the routing matrix beside the strips");

    const double stemBpm = stems.stemBpm();
    const double transportBpm = engine_ ? engine_->transport().bpm() : 120.0;
    const double effective = stemBpm > 0.0 ? stemBpm : transportBpm;

    char tempoText[64];
    std::snprintf(tempoText, sizeof(tempoText), "%.2f bpm", effective);
    list.addTextClipped(ui.font(t.fontSmall), tempoRow.removeFromLeft(tempoRow.width * 0.30f),
                        stemBpm > 0.0 ? t.text : t.textFaint, tempoText);

    if (ui.button(ui.idFrom(&node, 420), tempoRow.removeFromLeft(tempoRow.width * 0.44f),
                  "detect", Ui::ButtonStyle::Normal)) {
        int bars = 0;
        const double detected = stems.detectBpm(
            engine_ ? engine_->transport().snapshot().timeSigNumerator : 4, &bars);
        if (detected > 0.0) {
            stems.setStemBpm(detected);
            ui.notify("detected " + std::to_string(static_cast<int>(detected + 0.5))
                          + " bpm over " + std::to_string(bars) + " bars",
                      t.accent, 3.0f);
        } else {
            ui.notify("could not work a tempo out of that length", t.danger, 4.0f);
        }
    }
    if (ui.isHot(ui.idFrom(&node, 420)))
        ui.setTooltip("Work the tempo back from the length of the longest stem");

    tempoRow.removeFromLeft(s * 4.0f);

    // Pushes the stem tempo onto the project, which is what you want once the
    // stems are the thing everything else has to line up with.
    if (ui.button(ui.idFrom(&node, 421), tempoRow, "to project",
                  Ui::ButtonStyle::Normal, false, engine_ != nullptr && effective > 0.0)) {
        if (engine_) {
            engine_->transport().setBpm(effective);
            ui.notify("project tempo set to " + std::to_string(static_cast<int>(effective + 0.5))
                          + " bpm", t.accent, 2.5f);
        }
    }
    if (ui.isHot(ui.idFrom(&node, 421)))
        ui.setTooltip("Set the project tempo from this stem player");


    area.removeFromTop(s * stemBody::kAfterTempo);

    // -- stem strips -------------------------------------------------------
    // -- routing matrix ----------------------------------------------------
    // Carved off the right before anything else is laid out, so opening it
    // never reflows the strips.
    Rect matrixArea;
    if (stems.matrixOpen) {
        matrixArea = area.removeFromRight(s * 244.0f);
        area.removeFromRight(s * 6.0f);
    }

    // The bar the whole song spans, so section markers can be laid on the
    // strips in the same coordinates the playheads move through.
    const double songSeconds = [&] {
        double longest = 0.0;
        for (int i = 0; i < kMaxStems; ++i)
            if (const auto buffer = stems.stem(i)) longest = std::max(longest, buffer->durationSeconds());
        return longest;
    }();

    const double stemBpmForMarkers = stems.stemBpm() > 0.0
        ? stems.stemBpm()
        : (engine_ ? engine_->transport().bpm() : 120.0);
    const double beatsPerBarMarkers = engine_
        ? static_cast<double>(std::max(1, engine_->transport().snapshot().timeSigNumerator)) : 4.0;

    for (int slot = 0; slot < kMaxStems; ++slot) {
        if (area.height < s * (stemBody::kStripHeight + 1.0f)) break;
        Rect row = area.removeFromTop(s * stemBody::kStripHeight);

        const bool loaded = stems.stemLoaded(slot);

        const Rect muteArea = row.removeFromLeft(s * 20.0f);
        ui.parameterToggle(muteArea, node.parameter(
            node.indexOfParameter("mute" + std::to_string(slot + 1))), t.danger);
        row.removeFromLeft(s * 3.0f);

        const Rect nameArea = row.removeFromLeft(row.width * 0.30f);
        list.addTextClipped(ui.font(t.fontSmall), nameArea,
                            loaded ? t.textDim : t.textFaint,
                            loaded ? stems.stemName(slot)
                                   : std::string("drop a stem here"));

        // -- spectral strip ------------------------------------------------
        // Red where the energy is low, blue where it is high, brightness by
        // level. Eight identically-named waveforms tell you nothing; eight
        // strips coloured by content tell you which one is the bass at a glance,
        // and which one the colour engine is about to do something drastic to.
        const Rect strip = row.deflated(s * 1.0f);
        list.addRectFilled(strip, t.panelSunken, 2.0f);

        const auto& spectrum = stems.spectrum(slot);
        if (!spectrum.empty() && strip.width > 2.0f) {
            const int columns = std::min(static_cast<int>(strip.width),
                                         static_cast<int>(spectrum.size()));
            const float columnWidth = strip.width / static_cast<float>(std::max(1, columns));

            for (int c = 0; c < columns; ++c) {
                const std::size_t index = static_cast<std::size_t>(
                    static_cast<float>(c) / static_cast<float>(columns)
                    * static_cast<float>(spectrum.size()));
                const auto& band = spectrum[std::min(index, spectrum.size() - 1)];

                const float total = band.low + band.mid + band.high;
                if (total < 1.0e-4f) continue;

                // Hue from the balance of the three bands, value from the sum.
                const float lowShare = band.low / total;
                const float highShare = band.high / total;
                const float level = clampValue(total, 0.0f, 1.0f);

                const Colour colour{ 0.30f + lowShare * 0.70f,
                                     0.28f + (1.0f - std::abs(lowShare - highShare)) * 0.30f,
                                     0.35f + highShare * 0.65f,
                                     0.25f + level * 0.75f };

                list.addRectFilled(Rect{ strip.left() + columnWidth * static_cast<float>(c),
                                         strip.top(), columnWidth + 0.5f, strip.height },
                                   colour);
            }
        }

        // The live meter rides on top as a thin underline, so it does not hide
        // the spectrum it is drawn over.
        const float level = std::max(stems.meterLevel(slot, 0), stems.meterLevel(slot, 1));
        if (level > 0.0f) {
            list.addRectFilled(Rect{ strip.left(), strip.bottom() - s * 2.0f,
                                     strip.width * clampValue(level, 0.0f, 1.0f), s * 2.0f },
                               level > 0.95f ? t.danger : t.accent);
        }
        // -- section markers -----------------------------------------------
        // The same colour the section grid uses, so a glance says where in the
        // song each section sits and how long it is. Drawn over the spectrum
        // rather than beside it: the point is the relationship between the two.
        if (songSeconds > 0.0 && loaded) {
            for (int i = 0; i < stems.sectionCount(); ++i) {
                const StemSection& section = stems.sections()[static_cast<std::size_t>(i)];

                const double startSeconds = static_cast<double>(section.startBar)
                                          * beatsPerBarMarkers * 60.0 / stemBpmForMarkers;
                const double lengthSeconds = static_cast<double>(std::max(1, section.lengthBars))
                                           * beatsPerBarMarkers * 60.0 / stemBpmForMarkers;
                if (startSeconds >= songSeconds) continue;

                const float x0 = strip.left()
                    + strip.width * static_cast<float>(startSeconds / songSeconds);
                const float x1 = strip.left()
                    + strip.width * static_cast<float>(std::min(1.0, (startSeconds + lengthSeconds) / songSeconds));

                const Colour marker = sectionColour(section, i);
                const bool isActive = i == active;

                // A tick at the boundary, and a thin band along the top edge
                // showing the extent - enough to read, little enough to leave
                // the spectrum visible underneath.
                list.addRectFilled(Rect{ x0, strip.top(), std::max(1.0f, x1 - x0), s * 2.5f },
                                   marker.withAlpha(isActive ? 0.95f : 0.45f));
                list.addRectFilled(Rect{ x0, strip.top(), 1.0f, strip.height },
                                   marker.withAlpha(isActive ? 0.8f : 0.35f));
            }
        }

        // -- snippet selection -----------------------------------------------
        // Shift-drag along a strip marks the range to hand to the build
        // generator. Done here rather than in a dialog because the only way to
        // pick a useful grain source is against the picture of the sound.
        if (loaded && songSeconds > 0.0) {
            const UiId snipId = ui.idFrom(&node, 1500 + slot);
            bool snipHovered = false, snipHeld = false;
            ui.buttonBehaviour(snipId, strip, snipHovered, snipHeld);

            if (ui.isActive(snipId) && ui.input().shift) {
                const float a = clampValue((ui.dragStart().x - strip.left())
                                               / std::max(1.0f, strip.width), 0.0f, 1.0f);
                const float b = clampValue((ui.input().mousePosition.x - strip.left())
                                               / std::max(1.0f, strip.width), 0.0f, 1.0f);

                StemPlayerNode::Snippet snip;
                snip.slot = slot;
                snip.startSeconds = std::min(a, b) * songSeconds;
                snip.lengthSeconds = std::abs(b - a) * songSeconds;
                snip.tempoMatched = !ui.input().alt;   // alt gives a free-form range
                stems.setSnippet(snip);
            }
            if (snipHovered && ui.input().shift)
                ui.setCursor(Cursor::ResizeHorizontal);
        }

        // The current selection, drawn on whichever strip it came from.
        if (stems.snippet().slot == slot && stems.snippet().lengthSeconds > 0.0
            && songSeconds > 0.0) {
            const float x0 = strip.left()
                + strip.width * static_cast<float>(stems.snippet().startSeconds / songSeconds);
            const float x1 = strip.left()
                + strip.width * static_cast<float>((stems.snippet().startSeconds
                                                    + stems.snippet().lengthSeconds) / songSeconds);

            list.addRectFilled(Rect{ x0, strip.top(), std::max(2.0f, x1 - x0), strip.height },
                               t.warning.withAlpha(0.28f));
            list.addRect(Rect{ x0, strip.top(), std::max(2.0f, x1 - x0), strip.height },
                         t.warning, 1.0f, 0.0f);
        }

        // -- playhead --------------------------------------------------------
        // One per stem, because a stem the build node is chopping is
        // deliberately not where the others are. Seeing them separate is how
        // you know the chop is on and which stems it caught.
        if (loaded) {
            const float head = stems.stemPlayhead(slot);
            if (head > 0.0f) {
                const float x = strip.left() + strip.width * clampValue(head, 0.0f, 1.0f);
                const bool chopped = (stems.chopMask() & (1u << slot)) != 0
                                  && stems.parameter(node.indexOfParameter("repeat")).boolValue();
                list.addRectFilled(Rect{ x - 1.0f, strip.top(), 2.0f, strip.height },
                                   chopped ? t.danger : t.text);
            }
        }

        list.addRect(strip, t.border.withAlpha(0.5f), 1.0f, 2.0f);
    }

    if (stems.matrixOpen) drawStemMatrix(ui, stems, matrixArea);
}

void PatcherView::drawStemMatrix(Ui& ui, StemPlayerNode& stems, const Rect& bounds) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(bounds, t.panelSunken, t.cornerRadius);
    list.addRect(bounds, t.border, 1.0f, t.cornerRadius);

    Rect area = bounds.deflated(s * 4.0f);
    list.addTextClipped(ui.font(t.fontSmall), area.removeFromTop(s * 14.0f), t.textDim,
                        "routing");

    // Column headings: one per output. A stem lands on whichever cell is lit,
    // and several stems lighting the same column is the normal case rather than
    // a clash - that is what a bus is.
    Rect headerRow = area.removeFromTop(s * 12.0f);
    headerRow.removeFromLeft(s * 52.0f);
    const float cellWidth = headerRow.width / static_cast<float>(kMaxStems);

    for (int output = 0; output < kMaxStems; ++output) {
        list.addTextClipped(ui.font(t.fontSmall), headerRow.removeFromLeft(cellWidth),
                            t.textFaint, std::to_string(output + 1),
                            DrawList::Align::Centre);
    }

    area.removeFromTop(s * 2.0f);

    for (int slot = 0; slot < kMaxStems; ++slot) {
        if (area.height < s * 20.0f) break;
        Rect row = area.removeFromTop(s * 19.0f);

        const bool loaded = stems.stemLoaded(slot);
        const int resolved = stems.resolvedRoute(slot);
        const bool overridden = stems.stemRoute(slot) >= 0;

        // The per-stem level, right where the routing is decided - the two
        // questions asked together when a bus is being balanced.
        const Rect knob = row.removeFromLeft(s * 22.0f);
        ui.parameterKnob(knob, stems.parameter(
            stems.indexOfParameter("gain" + std::to_string(slot + 1))), t.accentDim);

        const Rect nameArea = row.removeFromLeft(s * 30.0f);
        list.addTextClipped(ui.font(t.fontSmall), nameArea,
                            loaded ? t.textDim : t.textFaint,
                            stems.stemName(slot).substr(0, 3));

        const float width = row.width / static_cast<float>(kMaxStems);
        for (int output = 0; output < kMaxStems; ++output) {
            Rect cell = row.removeFromLeft(width).deflated(s * 1.5f);

            const bool on = resolved == output;
            bool hovered = false, held = false;
            if (ui.buttonBehaviour(ui.idFrom(&stems, 1200 + slot * kMaxStems + output),
                                   cell, hovered, held)) {
                // Clicking the lit cell clears the override and hands the stem
                // back to its tag; clicking any other sets one.
                stems.setStemRoute(slot, on && overridden ? -1 : output);
            }

            Colour fill = on ? (overridden ? t.control.withAlpha(0.7f) : t.accent.withAlpha(0.6f))
                             : t.widgetBackground;
            if (hovered) fill = fill.brightened(1.4f);
            list.addRectFilled(cell, fill, 2.0f);

            if (hovered) {
                ui.setTooltip(on && overridden
                    ? "Pinned here - click to hand it back to the tag"
                    : on ? "Routed here by its tag"
                         : "Pin " + stems.stemName(slot) + " to output "
                               + std::to_string(output + 1));
            }
        }
    }
}

void PatcherView::drawColorBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& color = static_cast<ColorNode&>(node);

    Rect area = body;

    // -- the axis ----------------------------------------------------------
    // Drawn as an actual red-to-blue gradient with a detent in the middle, so
    // the knob's meaning needs no label and neutral is findable by eye.
    Rect axis = area.removeFromTop(s * 30.0f).deflated(s * 2.0f);

    constexpr int kBands = 24;
    const float bandWidth = axis.width / static_cast<float>(kBands);
    for (int i = 0; i < kBands; ++i) {
        const float position = (static_cast<float>(i) + 0.5f) / static_cast<float>(kBands);
        const float signed01 = position * 2.0f - 1.0f;

        // Toward red one way, toward blue the other, desaturating through the
        // middle where the chain is untouched.
        const Colour red{ 0.85f, 0.22f, 0.20f, 1.0f };
        const Colour blue{ 0.20f, 0.45f, 0.90f, 1.0f };
        const Colour neutral = t.widgetTrack;

        const Colour end = signed01 < 0.0f ? red : blue;
        const float amount = std::abs(signed01);
        const Colour band{ neutral.r + (end.r - neutral.r) * amount,
                           neutral.g + (end.g - neutral.g) * amount,
                           neutral.b + (end.b - neutral.b) * amount, 1.0f };

        list.addRectFilled(Rect{ axis.left() + bandWidth * static_cast<float>(i),
                                 axis.top(), bandWidth + 0.5f, axis.height }, band);
    }
    list.addRect(axis, t.border, 1.0f, 0.0f);

    // The centre detent.
    list.addRectFilled(Rect{ axis.centre().x - 0.5f, axis.top(), 1.0f, axis.height },
                       t.textFaint);

    Parameter& colorParam = node.parameter(node.indexOfParameter(ColorNode::kColorParam));

    bool hovered = false, held = false;
    const UiId axisId = ui.idFrom(&node, 500);
    ui.buttonBehaviour(axisId, axis, hovered, held);
    if (ui.isActive(axisId)) {
        const float position = clampValue((ui.input().mousePosition.x - axis.left())
                                              / std::max(1.0f, axis.width), 0.0f, 1.0f);
        float value = position * 2.0f - 1.0f;
        // Snaps to neutral near the middle: the one position that has to be
        // exact is the one that means "leave it alone".
        if (std::abs(value) < 0.04f) value = 0.0f;
        colorParam.setValue(value);
    }
    if (hovered) ui.setCursor(Cursor::ResizeHorizontal);

    // The handle.
    const float current = colorParam.value();
    const float handleX = axis.left() + axis.width * (current * 0.5f + 0.5f);
    list.addRectFilled(Rect{ handleX - 2.0f, axis.top() - 2.0f, 4.0f, axis.height + 4.0f },
                       t.text, 1.0f);

    area.removeFromTop(s * 4.0f);

    // -- readout -----------------------------------------------------------
    char readout[96];
    std::snprintf(readout, sizeof(readout), "%s  %.0f%%   %d targets",
                  current < -0.01f ? "red" : current > 0.01f ? "blue" : "neutral",
                  std::abs(current) * 100.0f, static_cast<int>(color.targets().size()));
    list.addTextClipped(ui.font(t.fontSmall), area.removeFromTop(s * 14.0f),
                        current == 0.0f ? t.textFaint : t.text, readout,
                        DrawList::Align::Centre);

    area.removeFromTop(s * 3.0f);

    // -- capture -----------------------------------------------------------
    Rect captureRow = area.removeFromTop(s * 20.0f);
    const float third = captureRow.width / 3.0f;

    if (engine_) {
        if (ui.button(ui.idFrom(&node, 501), captureRow.removeFromLeft(third).deflated(s * 1.0f),
                      "set red", Ui::ButtonStyle::Normal))
            color.captureEnd(ColorNode::End::Red, engine_->graph());
        if (ui.button(ui.idFrom(&node, 502), captureRow.removeFromLeft(third).deflated(s * 1.0f),
                      "set mid", Ui::ButtonStyle::Normal))
            color.captureEnd(ColorNode::End::Neutral, engine_->graph());
        if (ui.button(ui.idFrom(&node, 503), captureRow.deflated(s * 1.0f),
                      "set blue", Ui::ButtonStyle::Normal))
            color.captureEnd(ColorNode::End::Blue, engine_->graph());
    }

    area.removeFromTop(s * 3.0f);
    if (area.height >= s * 16.0f) {
        Rect depthRow = area.removeFromTop(s * 16.0f);
        ui.parameterSlider(depthRow, node.parameter(node.indexOfParameter("depth")), t.accentDim);
    }
}

void PatcherView::drawBuildBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& build = static_cast<BuildNode&>(node);

    Rect area = body;

    // -- the switch --------------------------------------------------------
    // Momentary, deliberately: it engages on press and releases on release,
    // rather than toggling. A build you have to remember to turn off is a build
    // that gets left on.
    Rect switchArea = area.removeFromTop(s * 56.0f).deflated(s * 2.0f);

    Parameter& engage = node.parameter(node.indexOfParameter(BuildNode::kEngageParam));

    bool hovered = false, held = false;
    const UiId switchId = ui.idFrom(&node, 600);
    ui.buttonBehaviour(switchId, switchArea, hovered, held);

    const bool down = ui.isActive(switchId);
    engage.setValue(down ? 1.0f : 0.0f);

    const float progress = build.progress();
    const bool running = build.engaged();

    Colour fill = running ? t.danger.withAlpha(0.35f) : t.widgetBackground;
    if (hovered && !running) fill = fill.brightened(1.4f);
    list.addRectFilled(switchArea, fill, t.cornerRadius);

    if (running) {
        list.addRectFilled(Rect{ switchArea.left(), switchArea.bottom() - s * 4.0f,
                                 switchArea.width * progress, s * 4.0f }, t.danger);
        list.addGlow(switchArea, t.danger.withAlpha(0.30f), 8.0f, t.cornerRadius, 4);
    }
    list.addRect(switchArea, running ? t.danger : t.border, 1.0f, t.cornerRadius);

    list.addTextClipped(ui.font(t.fontUiBold), switchArea,
                        running ? t.text : t.textDim,
                        running ? "BUILDING" : "hold to build", DrawList::Align::Centre);

    if (hovered) ui.setTooltip("Hold. Releases on the next bar so the drop lands in time.");

    area.removeFromTop(s * 5.0f);

    // -- the stutter, from and to ------------------------------------------
    // Laid out as the sentence it is: chop from a quarter to a sixteenth, over
    // eight bars, on this curve. Cycling controls rather than dropdowns - the
    // menu a combo opens falls outside the node and has to be aimed at while
    // the set is running, which is the wrong ask of a performance control.
    // One caption column down the left, so the three rows read as three lines of
    // one sentence. The controls themselves show only their value: the caption
    // has already said what they are, and repeating "Build Length" inside a
    // slider that is forty pixels wide only costs the number its room.
    const float caption = s * 36.0f;
    const float rowHeight = s * 20.0f;
    const float gap = s * 5.0f;

    auto captionise = [&](Rect& row, const char* text) {
        list.addTextClipped(ui.font(t.fontSmall), row.removeFromLeft(caption),
                            t.textFaint, text);
    };

    Rect chopRow = area.removeFromTop(rowHeight);
    captionise(chopRow, "chop");
    ui.parameterCycle(chopRow.removeFromLeft(chopRow.width * 0.42f),
                      node.parameter(node.indexOfParameter("startDivide")), t.control);
    list.addTextClipped(ui.font(t.fontSmall), chopRow.removeFromLeft(s * 18.0f),
                        t.textFaint, "to", DrawList::Align::Centre);
    ui.parameterCycle(chopRow, node.parameter(node.indexOfParameter("endDivide")), t.danger);

    area.removeFromTop(gap);

    Rect shapeRow = area.removeFromTop(rowHeight);
    captionise(shapeRow, "over");
    ui.parameterSlider(shapeRow.removeFromLeft(shapeRow.width * 0.42f),
                       node.parameter(node.indexOfParameter("bars")), t.control, false);
    shapeRow.removeFromLeft(s * 6.0f);
    ui.parameterCycle(shapeRow, node.parameter(node.indexOfParameter("curve")), t.accent);

    area.removeFromTop(gap);

    Rect releaseRow = area.removeFromTop(rowHeight);
    captionise(releaseRow, "drop");
    ui.parameterCycle(releaseRow.removeFromLeft(releaseRow.width * 0.52f),
                      node.parameter(node.indexOfParameter("release")), t.accent);
    releaseRow.removeFromLeft(s * 6.0f);
    ui.parameterSlider(releaseRow, node.parameter(node.indexOfParameter("colorPush")),
                       t.accentDim, false);

    area.removeFromTop(s * 5.0f);

    // -- the snippet -------------------------------------------------------
    // The range dragged out on a stem strip, drawn big enough to trim exactly.
    // The selection is made in context against the sound; this is where it is
    // made precise.
    if (area.height >= s * 60.0f) {
        Rect snippetArea = area.removeFromTop(s * 46.0f);
        list.addRectFilled(snippetArea, t.panelSunken, 2.0f);
        list.addRect(snippetArea, t.border, 1.0f, 2.0f);

        const auto snippet = build.snippet();
        if (!snippet || snippet->empty()) {
            list.addTextClipped(ui.font(t.fontSmall), snippetArea, t.textFaint,
                                "shift-drag a stem strip, then send it here",
                                DrawList::Align::Centre);
        } else {
            Rect plot = snippetArea.deflated(s * 3.0f);

            const auto& overview = snippet->overview();
            if (!overview.minimum.empty()) {
                const float centreY = plot.centre().y;
                const float half = plot.height * 0.44f;
                const int columns = static_cast<int>(plot.width);

                for (int x = 0; x < columns; ++x) {
                    const std::size_t index = static_cast<std::size_t>(
                        static_cast<float>(x) / static_cast<float>(std::max(1, columns))
                        * static_cast<float>(overview.minimum.size()));
                    if (index >= overview.minimum.size()) break;

                    const float low = overview.minimum[index];
                    const float high = overview.maximum[index];
                    list.addRectFilled(Rect{ plot.left() + static_cast<float>(x),
                                             centreY - high * half, 1.0f,
                                             std::max(1.0f, (high - low) * half) },
                                       t.warning.withAlpha(0.7f));
                }
            }

            // The trimmed-away ends are dimmed rather than hidden, so what is
            // being excluded stays visible while it is being chosen.
            const float x0 = plot.left() + plot.width * build.trimStart();
            const float x1 = plot.left() + plot.width * build.trimEnd();

            list.addRectFilled(Rect{ plot.left(), plot.top(), x0 - plot.left(), plot.height },
                               t.background.withAlpha(0.72f));
            list.addRectFilled(Rect{ x1, plot.top(), plot.right() - x1, plot.height },
                               t.background.withAlpha(0.72f));

            // Two handles, dragged independently.
            for (int handle = 0; handle < 2; ++handle) {
                const float x = handle == 0 ? x0 : x1;
                const Rect grip{ x - s * 4.0f, plot.top(), s * 8.0f, plot.height };

                bool gripHovered = false, gripHeld = false;
                const UiId gripId = ui.idFrom(&node, 660 + handle);
                ui.buttonBehaviour(gripId, grip, gripHovered, gripHeld);

                if (ui.isActive(gripId)) {
                    const float value = clampValue((ui.input().mousePosition.x - plot.left())
                                                       / std::max(1.0f, plot.width), 0.0f, 1.0f);
                    if (handle == 0) build.setTrim(value, build.trimEnd());
                    else build.setTrim(build.trimStart(), value);
                }
                if (gripHovered) ui.setCursor(Cursor::ResizeHorizontal);

                list.addRectFilled(Rect{ x - 1.0f, plot.top(), 2.0f, plot.height },
                                   gripHovered ? t.text : t.warning);
            }

            char label[128];
            std::snprintf(label, sizeof(label), "%s  %.2fs",
                          build.snippetLabel().c_str(),
                          snippet->durationSeconds()
                              * static_cast<double>(build.trimEnd() - build.trimStart()));
            list.addTextClipped(ui.font(t.fontSmall),
                                Rect{ snippetArea.left() + s * 4.0f, snippetArea.bottom() - s * 12.0f,
                                      snippetArea.width - s * 8.0f, s * 11.0f },
                                t.textFaint, label);
        }

        area.removeFromTop(s * 4.0f);

        // Two per row, each with its own short caption. Four parameter sliders
        // carrying their full names across half a node's width is how "Spread"
        // became "Spr..." and "Grains" became "Gr..." - the name outside the
        // control leaves the whole slider for the number.
        auto grainPair = [&](const char* leftCaption, const char* leftId,
                             const char* rightCaption, const char* rightId) {
            Rect row = area.removeFromTop(s * 18.0f);
            Rect left = row.removeFromLeft(row.width * 0.5f - s * 3.0f);
            row.removeFromLeft(s * 6.0f);

            list.addTextClipped(ui.font(t.fontSmall), left.removeFromLeft(s * 30.0f),
                                t.textFaint, leftCaption);
            ui.parameterSlider(left, node.parameter(node.indexOfParameter(leftId)),
                               t.warning, false);

            list.addTextClipped(ui.font(t.fontSmall), row.removeFromLeft(s * 30.0f),
                                t.textFaint, rightCaption);
            ui.parameterSlider(row, node.parameter(node.indexOfParameter(rightId)),
                               t.warning, false);
        };

        grainPair("size", "grainSize", "dens", "grainDensity");
        area.removeFromTop(s * 3.0f);
        grainPair("wide", "grainSpread", "level", "grainGain");

        area.removeFromTop(s * 4.0f);
    }

    // -- which stems get chopped -------------------------------------------
    // Eight little pads named after the stem slots. Chopping everything is a
    // fault rather than an effect, so which stems the stutter catches has to be
    // one press away, not buried in the inspector.
    if (area.height >= s * 22.0f) {
        Rect stemRow = area.removeFromTop(s * 20.0f);

        const StemPlayerNode* stems = engine_
            ? dynamic_cast<const StemPlayerNode*>(engine_->graph().node(build.stemPlayer()))
            : nullptr;

        const float padWidth = stemRow.width / static_cast<float>(kMaxStems);
        for (int slot = 0; slot < kMaxStems; ++slot) {
            Rect pad = stemRow.removeFromLeft(padWidth).deflated(s * 1.0f);

            const bool on = build.chopsStem(slot);
            bool padHovered = false, padHeld = false;
            if (ui.buttonBehaviour(ui.idFrom(&node, 640 + slot), pad, padHovered, padHeld))
                build.toggleStem(slot);

            Colour padFill = on ? t.danger.withAlpha(0.42f) : t.widgetBackground;
            if (padHovered) padFill = padFill.brightened(1.4f);
            list.addRectFilled(pad, padFill, 2.0f);
            list.addRect(pad, on ? t.danger : t.border, 1.0f, 2.0f);

            // The stem's own initial when a player is wired, so the pads mean
            // something specific rather than being eight numbered boxes.
            const std::string name = stems ? stems->stemName(slot) : std::string();
            const std::string label = name.empty() ? std::to_string(slot + 1)
                                                   : name.substr(0, 2);
            list.addTextClipped(ui.font(t.fontSmall), pad, on ? t.text : t.textFaint,
                                label, DrawList::Align::Centre);

            if (padHovered && !name.empty())
                ui.setTooltip(name + (on ? " - chopped by the build" : " - plays straight through"));
        }
    }

    // A build with nothing wired to it is silent and confusing, so say so.
    if (build.stemPlayer() == kInvalidNode && build.colorNode() == kInvalidNode
        && area.height >= s * 14.0f) {
        list.addTextClipped(ui.font(t.fontSmall), area.removeFromTop(s * 14.0f), t.textFaint,
                            "not wired - pick targets in the inspector",
                            DrawList::Align::Centre);
    }
}

void PatcherView::drawDropBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& drop = static_cast<DropNode&>(node);

    Rect area = body;

    // -- fire --------------------------------------------------------------
    // A drop node wired to a build is fired by the build; this button is for
    // hearing the impact while it is being balanced, which is the only time
    // anyone wants to trigger one by hand.
    Rect fireArea = area.removeFromTop(s * dropBody::kFireRow).deflated(s * 2.0f);

    bool fireHovered = false, fireHeld = false;
    const UiId fireId = ui.idFrom(&node, 700);
    if (ui.buttonBehaviour(fireId, fireArea, fireHovered, fireHeld)) drop.trigger();

    const float progress = drop.progress();
    const bool sounding = progress >= 0.0f;

    Colour fill = sounding ? t.warning.withAlpha(0.30f) : t.widgetBackground;
    if (fireHovered) fill = fill.brightened(1.4f);
    list.addRectFilled(fireArea, fill, t.cornerRadius);
    list.addRect(fireArea, sounding ? t.warning : t.border, 1.0f, t.cornerRadius);
    list.addTextClipped(ui.font(t.fontUiBold), fireArea, sounding ? t.text : t.textDim,
                        "fire", DrawList::Align::Centre);
    if (fireHovered) ui.setTooltip("Fire every layer now. The build fires it in performance.");

    area.removeFromTop(s * dropBody::kAfterFire);

    // -- layers ------------------------------------------------------------
    for (int i = 0; i < DropNode::kLayers; ++i) {
        if (area.height < s * 24.0f) break;

        // Tall enough for a button: a control's label needs its font height
        // plus the padding the button style puts around it, and 13 units of row
        // clipped the mute's "m" to a bare box.
        Rect row = area.removeFromTop(s * dropBody::kLayerHeight);
        list.addRectFilled(row, t.panelSunken, 2.0f);

        Rect inner = row.deflated(s * 3.0f);
        Rect nameRow = inner.removeFromTop(s * 19.0f);

        const Rect muteArea = nameRow.removeFromRight(s * 18.0f);
        Parameter& mute = node.parameter(node.indexOfParameter(
            std::string(i == 0 ? "a" : i == 1 ? "b" : "c") + "Mute"));
        const bool muted = mute.boolValue();
        // An icon rather than a letter: a one-character label inside a button
        // the width of a fader thumb has less room for its glyph than the glyph
        // needs, and came out as an ellipsis. An icon is drawn to fit the rect.
        if (ui.iconButton(ui.idFrom(&node, 720 + i), muteArea, Ui::Icon::Power,
                          muted ? t.danger : t.textFaint, muted))
            mute.setValue(muted ? 0.0f : 1.0f);
        if (ui.isHot(ui.idFrom(&node, 720 + i)))
            ui.setTooltip(muted ? "Muted - this layer stays out of the impact"
                                : "Mute this layer");

        const std::string name = drop.layerName(i);
        list.addTextClipped(ui.font(t.fontSmall), nameRow,
                            name.empty() ? t.textFaint : (muted ? t.textFaint : t.textDim),
                            name.empty() ? "drop a sample here" : name);

        if (inner.height < s * 14.0f) continue;

        // Gain and pitch side by side: the two that get moved while listening.
        Rect controls = inner.removeFromTop(s * 14.0f);
        const std::string prefix = i == 0 ? "a" : i == 1 ? "b" : "c";

        ui.parameterSlider(controls.removeFromLeft(controls.width * 0.5f - s * 2.0f),
                           node.parameter(node.indexOfParameter(prefix + "Gain")),
                           t.accent, false);
        controls.removeFromLeft(s * 4.0f);
        ui.parameterSlider(controls, node.parameter(node.indexOfParameter(prefix + "Pitch")),
                           t.control, false);

        area.removeFromTop(s * 3.0f);
    }

    // A drop with no build behind it never fires on its own, which is worth
    // saying on the node rather than leaving to be discovered mid-set.
    if (drop.buildNode() == kInvalidNode && area.height >= s * 13.0f) {
        list.addTextClipped(ui.font(t.fontSmall), area.removeFromTop(s * 13.0f), t.textFaint,
                            "no build wired - pick one in the inspector",
                            DrawList::Align::Centre);
    }
}

void PatcherView::drawMixerBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    auto& mixer = static_cast<MixerNode&>(node);

    Rect area = body;
    const int channels = mixer.channelCount();
    const float stripWidth = area.width / static_cast<float>(channels + 1);

    for (int channel = 0; channel < channels; ++channel) {
        Rect strip{ area.left() + stripWidth * static_cast<float>(channel), area.top(),
                    stripWidth - 3.0f, area.height };

        const Rect faderArea = strip.removeFromTop(strip.height - 34.0f);
        const Rect meterArea = faderArea.deflated(s * 0.0f).removeFromRight(s * 6.0f);
        Rect gainArea = faderArea;
        gainArea.removeFromRight(s * 8.0f);

        Parameter& gain = node.parameter(mixer.gainParam(channel));
        float normalised = gain.normalised();
        const bool silenced = mixer.channelSilencedBySolo(channel);

        if (ui.sliderNormalised(ui.idFrom(&node, 100 + channel), gainArea, normalised,
                                silenced ? t.textFaint : t.accent, true))
            gain.setNormalised(normalised);

        ui.stereoMeter(meterArea, mixer.channelMeter(channel, 0), mixer.channelMeter(channel, 1), true);

        Rect buttons = strip.removeFromTop(s * 16.0f);
        const Rect muteArea = buttons.removeFromLeft(buttons.width * 0.5f - 1.0f);
        buttons.removeFromLeft(s * 2.0f);

        Parameter& mute = node.parameter(mixer.muteParam(channel));
        Parameter& solo = node.parameter(mixer.soloParam(channel));

        if (ui.button(ui.idFrom(&node, 200 + channel), muteArea, "M",
                      Ui::ButtonStyle::Toggle, mute.boolValue()))
            mute.setValue(mute.boolValue() ? 0.0f : 1.0f);
        if (ui.button(ui.idFrom(&node, 300 + channel), buttons, "S",
                      Ui::ButtonStyle::Toggle, solo.boolValue()))
            solo.setValue(solo.boolValue() ? 0.0f : 1.0f);

        ui.labelDim(strip, std::to_string(channel + 1), DrawList::Align::Centre);
    }

    // Master strip on the right.
    Rect master{ area.left() + stripWidth * static_cast<float>(channels), area.top(),
                 stripWidth - 3.0f, area.height };
    const Rect masterFader = master.removeFromTop(master.height - 18.0f);
    const Rect masterMeter = masterFader.deflated(s * 0.0f).removeFromRight(s * 6.0f);
    Rect masterGainArea = masterFader;
    masterGainArea.removeFromRight(s * 8.0f);

    Parameter& masterGain = node.parameter(node.indexOfParameter("mastergain"));
    float masterNormalised = masterGain.normalised();
    if (ui.sliderNormalised(ui.idFrom(&node, 400), masterGainArea, masterNormalised, t.control, true))
        masterGain.setNormalised(masterNormalised);

    ui.stereoMeter(masterMeter, mixer.masterMeter(0), mixer.masterMeter(1), true);
    ui.labelDim(master, "M", DrawList::Align::Centre);
}

void PatcherView::drawPluginBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    DrawList& list = ui.draw();
    auto& plugin = static_cast<vst2::VstNode&>(node);

    Rect area = body;

    // -- status ------------------------------------------------------------
    Rect statusRow = area.removeFromTop(s * 14.0f);
    const auto& description = plugin.pluginDescription();

    std::string status = vst2::toString(description.architecture);
    if (plugin.bridged()) status += " - bridged";
    if (!plugin.pluginLoaded()) status = "not loaded";

    list.addTextClipped(ui.font(t.fontSmall), statusRow,
                        plugin.pluginLoaded() ? t.textFaint : t.danger, status);

    area.removeFromTop(s * 2.0f);

    // -- editor / reload ---------------------------------------------------
    Rect buttons = area.removeFromTop(s * 20.0f);
    const bool canEdit = plugin.pluginLoaded() && description.hasEditor;

    Rect editorArea = buttons.removeFromLeft(buttons.width - 24.0f);
    buttons.removeFromLeft(s * 4.0f);

    if (ui.button(ui.idFrom(&node, 40), editorArea,
                  plugin.editorOpen() ? "close editor" : "open editor",
                  Ui::ButtonStyle::Toggle, plugin.editorOpen(), canEdit))
        editorRequest_ = node.id();

    if (ui.iconButton(ui.idFrom(&node, 41), buttons, Ui::Icon::Refresh, t.textDim))
        plugin.reloadPlugin();
    if (ui.isHot(ui.idFrom(&node, 41)))
        ui.setTooltip("Reload the plugin, keeping its current state");

    area.removeFromTop(s * 3.0f);

    // -- the first few plugin parameters, as knobs -------------------------
    // Everything is reachable in the inspector; the node shows enough to
    // perform with without becoming a wall of controls.
    const int firstPluginParam = node.indexOfParameter("p0");
    if (firstPluginParam >= 0 && area.height > 34.0f) {
        constexpr int kInlineKnobs = 4;
        const float knobWidth = area.width / kInlineKnobs;

        for (int i = 0; i < kInlineKnobs; ++i) {
            const int index = firstPluginParam + i;
            if (index >= node.numParameters()) break;

            const Rect knobArea{ area.left() + knobWidth * static_cast<float>(i), area.top(),
                                 knobWidth - 2.0f, area.height };
            ui.parameterKnob(knobArea, node.parameter(index), t.categoryColour(node.category()), false);
        }
    } else if (area.height > 18.0f) {
        Rect mixRow = area.removeFromTop(s * 18.0f);
        ui.parameterSlider(mixRow, node.parameter(node.indexOfParameter("drywet")), t.accent, false);
    }
}

void PatcherView::drawGenericBody(Ui& ui, Node& node, const Rect& body) {
    const float s = bodyScale();
    const Theme& t = theme();
    const Colour accent = t.categoryColour(node.category());

    Rect area = body;
    const float rowHeight = s * 19.0f;

    for (int i = 0; i < node.numParameters(); ++i) {
        if (area.height < rowHeight) break;

        Parameter& parameter = node.parameter(i);
        if (!parameter.automatable()) continue;

        const Rect row = area.removeFromTop(rowHeight);

        switch (parameter.kind()) {
            case ParamKind::Bool:
                ui.parameterToggle(row, parameter, accent);
                break;
            case ParamKind::Choice:
                ui.parameterChoice(row, parameter);
                break;
            default:
                ui.parameterSlider(row, parameter, accent, true);
                break;
        }

        area.removeFromTop(s * 1.0f);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void PatcherView::handleInput(Ui& ui, const Rect& bounds) {
    const float z = viewScale();
    const InputState& input = ui.input();
    const bool overCanvas = ui.hovering(bounds);

    // -- zoom --------------------------------------------------------------
    if (overCanvas && input.wheel != 0.0f && !ui.pointerCaptured()) {
        // Zoom about the pointer, so the thing under the cursor stays put.
        const Vec2 anchorWorld = screenToWorld(input.mousePosition, bounds);
        setZoom(zoom_ * std::pow(1.12f, input.wheel));
        const Vec2 anchorAfter = screenToWorld(input.mousePosition, bounds);
        pan_ += anchorAfter - anchorWorld;
    }

    // -- what is under the pointer ----------------------------------------
    if (mode_ == CanvasMode::Idle && overCanvas && !ui.pointerCaptured()) {
        hovered_ = hitTestNodes(ui, bounds);
        hoveredCable_ = hovered_ == kInvalidNode ? hitTestCables(ui, bounds) : kInvalidConnection;
    } else if (mode_ != CanvasMode::Idle) {
        hoveredCable_ = kInvalidConnection;
    }

    const PortHit portHit = (mode_ == CanvasMode::Idle && overCanvas)
                                ? hitTestPorts(ui, bounds) : PortHit{};

    // -- begin interactions ------------------------------------------------
    if (mode_ == CanvasMode::Idle && overCanvas
        && input.mousePressed[static_cast<int>(MouseButton::Left)] && !ui.pointerCaptured()) {

        if (portHit.valid()) {
            // Dragging from a connected input detaches the existing cable, which
            // is how every patcher worth using behaves.
            if (portHit.isInput) {
                const Graph& graph = engine_->graph();
                for (const Connection& c : graph.connections()) {
                    if (c.destNode == portHit.node && c.destPort == portHit.port) {
                        cableSource_ = PortHit{ c.sourceNode, c.sourcePort, false };
                        engine_->graph().disconnect(c.id);
                        mode_ = CanvasMode::DraggingCable;
                        break;
                    }
                }
            }
            if (mode_ != CanvasMode::DraggingCable) {
                cableSource_ = portHit;
                mode_ = CanvasMode::DraggingCable;
            }
        } else if (hovered_ != kInvalidNode) {
            if (!isSelected(hovered_)) select(hovered_, input.shift);
            else if (input.shift) select(hovered_, true);

            // Only start a move if the grab was not on an inline control; the Ui
            // would have captured the pointer in that case.
            mode_ = CanvasMode::DraggingNodes;
            dragStartWorld_ = screenToWorld(input.mousePosition, bounds);
            dragOrigins_.clear();
            for (NodeId id : selection_) {
                if (const Node* node = engine_->graph().node(id))
                    dragOrigins_.emplace_back(id, Vec2{ node->canvasX, node->canvasY });
            }
        } else if (hoveredCable_ != kInvalidConnection) {
            // Clicking a cable selects nothing; alt-click cuts it.
            if (input.alt) {
                engine_->graph().disconnect(hoveredCable_);
                hoveredCable_ = kInvalidConnection;
            }
        } else {
            if (!input.shift) clearSelection();
            mode_ = CanvasMode::Marquee;
            marqueeStart_ = input.mousePosition;
        }
    }

    // Middle drag, or space+drag, pans. Right drag pans too, unless it opens the
    // context menu on release without moving.
    if (mode_ == CanvasMode::Idle && overCanvas
        && (input.mouseDown[static_cast<int>(MouseButton::Middle)]
            || (input.mouseDown[static_cast<int>(MouseButton::Left)] && input.keyDown(key::Space)))) {
        mode_ = CanvasMode::PanningCanvas;
    }

    // -- continue interactions --------------------------------------------
    switch (mode_) {
        case CanvasMode::PanningCanvas: {
            pan_ += input.mouseDelta / z;
            ui.setCursor(Cursor::Hand);
            if (!input.mouseDown[static_cast<int>(MouseButton::Middle)]
                && !input.mouseDown[static_cast<int>(MouseButton::Left)])
                mode_ = CanvasMode::Idle;
            break;
        }

        case CanvasMode::DraggingNodes: {
            const Vec2 world = screenToWorld(input.mousePosition, bounds);
            Vec2 delta = world - dragStartWorld_;

            // Ctrl snaps to the grid, which is the only time anyone wants it.
            for (const auto& [id, origin] : dragOrigins_) {
                if (Node* node = engine_->graph().node(id)) {
                    Vec2 target = origin + delta;
                    if (input.ctrl) {
                        target.x = std::round(target.x / kGridSpacing) * kGridSpacing;
                        target.y = std::round(target.y / kGridSpacing) * kGridSpacing;
                    }
                    node->canvasX = target.x;
                    node->canvasY = target.y;
                }
            }

            if (!input.mouseDown[static_cast<int>(MouseButton::Left)]) mode_ = CanvasMode::Idle;
            break;
        }

        case CanvasMode::DraggingCable: {
            if (!input.mouseDown[static_cast<int>(MouseButton::Left)]) {
                const PortHit target = hitTestPorts(ui, bounds);
                if (target.valid() && target.isInput != cableSource_.isInput) {
                    const PortHit source = cableSource_.isInput ? target : cableSource_;
                    const PortHit destination = cableSource_.isInput ? cableSource_ : target;

                    std::string reason;
                    if (engine_->graph().canConnect(source.node, source.port,
                                                    destination.node, destination.port, &reason)) {
                        engine_->graph().connect(source.node, source.port,
                                                 destination.node, destination.port);
                    } else if (!reason.empty()) {
                        ui.notify(reason, theme().warning, 2.5f);
                    }
                }
                cableSource_ = PortHit{};
                mode_ = CanvasMode::Idle;
            }
            break;
        }

        case CanvasMode::Marquee: {
            const Rect marquee = Rect::fromCorners(marqueeStart_, input.mousePosition);
            ui.draw().addRectFilled(marquee, theme().accent.withAlpha(0.08f));
            ui.draw().addRect(marquee, theme().accent.withAlpha(0.7f), 1.0f);

            if (!input.mouseDown[static_cast<int>(MouseButton::Left)]) {
                for (const auto& node : engine_->graph().nodes()) {
                    if (marquee.intersects(nodeBounds(*node, bounds)) && !isSelected(node->id()))
                        selection_.push_back(node->id());
                }
                mode_ = CanvasMode::Idle;
            }
            break;
        }

        case CanvasMode::ResizingNode: {
            if (Node* node = engine_->graph().node(resizingNode_)) {
                const Vec2 world = screenToWorld(input.mousePosition, bounds);
                node->canvasWidth = clampValue(resizeStartWidth_ + (world.x - dragStartWorld_.x),
                                               kNodeMinWidth, 900.0f);
            }
            ui.setCursor(Cursor::ResizeHorizontal);
            if (!input.mouseDown[static_cast<int>(MouseButton::Left)]) {
                mode_ = CanvasMode::Idle;
                resizingNode_ = kInvalidNode;
            }
            break;
        }

        case CanvasMode::Idle:
            break;
    }

    if (portHit.valid()) ui.setCursor(Cursor::Crosshair);
}

void PatcherView::handleShortcuts(Ui& ui, const Rect& bounds) {
    const InputState& input = ui.input();
    if (ui.keyboardCaptured()) return;

    if (input.keyPressed(key::Delete)) deleteSelection();
    if (input.ctrl && input.keyPressed(key::A)) selectAll();
    if (input.ctrl && input.keyPressed(key::D)) duplicateSelection();
    if (input.keyPressed(key::B)) bypassSelection();
    if (input.keyPressed(key::F1)) frameAll(bounds);
    if (input.keyPressed(key::Escape)) {
        clearSelection();
        mode_ = CanvasMode::Idle;
    }
}

void PatcherView::handlePortMenu(Ui& ui, const Rect& bounds) {
    const UiId menuId = ui.id("patcher.port");

    Rect popupRect;
    if (!ui.beginPopup(menuId, popupRect)) return;

    const Theme& t = theme();
    Rect area = popupRect.deflated(t.smallPadding);

    auto* stems = engine_ ? dynamic_cast<StemPlayerNode*>(engine_->graph().node(portMenuNode_))
                          : nullptr;
    if (!stems || portMenuSlot_ < 0) {
        ui.endPopup();
        ui.closePopup();
        return;
    }

    const int slot = portMenuSlot_;
    const std::size_t rackSize = downstreamChain(engine_->graph(), portMenuNode_, slot).size();

    // What this menu is about, said once: which stem, which output, how much is
    // on it. Without it the actions below could be about any of the eight.
    char header[96];
    std::snprintf(header, sizeof(header), "%s  -  out %d  -  %d effect%s",
                  stems->stemName(slot).c_str(), slot + 1,
                  static_cast<int>(rackSize), rackSize == 1 ? "" : "s");
    ui.labelDim(area.removeFromTop(t.scaled(15.0f)), header);
    ui.separator(area.removeFromTop(t.scaled(7.0f)));

    // -- save --------------------------------------------------------------
    if (savingChain_) {
        Rect row = area.removeFromTop(t.rowHeight);
        const Rect keepArea = row.removeFromRight(t.scaled(42.0f));
        const bool committed = ui.textField(ui.id("port.name"), row, chainNameBuffer_);
        if ((ui.button(ui.id("port.keep"), keepArea, "keep", Ui::ButtonStyle::Primary, false,
                       !chainNameBuffer_.empty())
             || committed)
            && !chainNameBuffer_.empty()) {
            if (onSaveChain) onSaveChain(portMenuNode_, slot, chainNameBuffer_);
            savingChain_ = false;
            chainNameBuffer_.clear();
            ui.closePopup();
        }
    } else {
        if (ui.button(ui.id("port.save"), area.removeFromTop(t.rowHeight), "save chain...",
                      Ui::ButtonStyle::Ghost, false, rackSize > 0)) {
            savingChain_ = true;
            chainNameBuffer_ = stems->stemName(slot);
            ui.beginTextEdit(ui.id("port.name"), chainNameBuffer_, true);
        }
        if (rackSize == 0 && ui.isHot(ui.id("port.save")))
            ui.setTooltip("Nothing on this stem to save");
    }

    // -- copy and paste ----------------------------------------------------
    // Armed here, dropped on another port. A menu cannot show a submenu of the
    // other seven stems without becoming a maze, and the two-step gesture is
    // the one the inspector already uses.
    const bool armed = copySourceNode_ == portMenuNode_ && copySourceSlot_ == slot;
    const bool pending = copySourceNode_ != kInvalidNode && !armed;

    if (ui.button(ui.id("port.copy"), area.removeFromTop(t.rowHeight),
                  armed ? "copying - pick a destination"
                        : pending ? "paste rack here" : "copy this rack",
                  Ui::ButtonStyle::Ghost, armed,
                  pending || rackSize > 0)) {
        if (armed) {
            copySourceNode_ = kInvalidNode;
            copySourceSlot_ = -1;
        } else if (pending) {
            if (onCopyChain && copySourceNode_ == portMenuNode_) {
                onCopyChain(portMenuNode_, copySourceSlot_, slot);
            } else if (copySourceNode_ != portMenuNode_) {
                // Across two players the ports are not interchangeable, and
                // copyStemChain only knows one node. Saying so beats copying
                // the wrong rack.
                ui.notify("a rack can only be copied within one stem player",
                          t.danger, 3.5f);
            }
            copySourceNode_ = kInvalidNode;
            copySourceSlot_ = -1;
            ui.closePopup();
        } else {
            copySourceNode_ = portMenuNode_;
            copySourceSlot_ = slot;
            ui.closePopup();
        }
    }

    if (ui.button(ui.id("port.tidy"), area.removeFromTop(t.rowHeight), "tidy the racks",
                  Ui::ButtonStyle::Ghost)) {
        tidyStemChains(portMenuNode_);
        ui.closePopup();
    }

    if (ui.button(ui.id("port.inspect"), area.removeFromTop(t.rowHeight),
                  "open in the inspector", Ui::ButtonStyle::Ghost)) {
        rackRequest_ = RackRequest{ portMenuNode_, slot };
        ui.closePopup();
    }

    // -- load --------------------------------------------------------------
    const std::vector<std::string> names = chainNames ? chainNames() : std::vector<std::string>{};
    ui.separator(area.removeFromTop(t.scaled(7.0f)));
    ui.labelDim(area.removeFromTop(t.scaled(14.0f)),
                names.empty() ? "no saved chains" : "load");

    for (const std::string& name : names) {
        if (area.height < t.rowHeight) break;
        if (ui.button(ui.id("port.load." + name), area.removeFromTop(t.rowHeight), name,
                      Ui::ButtonStyle::Ghost)) {
            if (onLoadChain) onLoadChain(portMenuNode_, slot, name);
            ui.closePopup();
        }
    }

    ui.endPopup();
}

void PatcherView::handleContextMenu(Ui& ui, const Rect& bounds) {
    const InputState& input = ui.input();
    const UiId menuId = ui.id("patcher.context");

    if (ui.hovering(bounds) && input.mousePressed[static_cast<int>(MouseButton::Right)]
        && !ui.popupOpen(menuId)) {
        contextMenuWorld_ = screenToWorld(input.mousePosition, bounds);

        // A right-click on one of a stem player's outputs is not a request for
        // the add-node palette - it is a request for that stem's rack. The rack
        // controls live in the inspector, so the gesture selects the player and
        // asks the inspector to open that slot rather than duplicating save,
        // load and tidy into a canvas menu.
        if (const PortHit port = hitTestPorts(ui, bounds);
            port.valid() && !port.isInput && engine_) {
            if (dynamic_cast<const StemPlayerNode*>(engine_->graph().node(port.node))) {
                select(port.node, false);
                portMenuNode_ = port.node;
                portMenuSlot_ = static_cast<int>(port.port);
                savingChain_ = false;
                chainNameBuffer_.clear();

                const std::size_t presets = chainNames ? chainNames().size() : 0;
                const float height = (7.0f + static_cast<float>(std::min<std::size_t>(presets, 8)))
                                   * theme().rowHeight + 40.0f;
                ui.openPopup(ui.id("patcher.port"), input.mousePosition,
                             { 232.0f, std::min(height, 460.0f) });
                return;
            }
        }

        // Right-clicking a node selects it first, so the menu acts on what was
        // clicked rather than on a stale selection.
        const NodeId under = hitTestNodes(ui, bounds);
        if (under != kInvalidNode && !isSelected(under)) select(under, false);

        const auto& types = NodeFactory::instance().types();
        const float height = static_cast<float>(types.size() + 4) * theme().rowHeight + 24.0f;
        ui.openPopup(menuId, input.mousePosition, { 210.0f, std::min(height, 520.0f) });
    }

    Rect popupRect;
    if (!ui.beginPopup(menuId, popupRect)) return;

    Rect area = popupRect.deflated(theme().smallPadding);
    const Theme& t = theme();

    // Actions on the current selection come first: they are what a right-click
    // on a node is usually for.
    if (!selection_.empty()) {
        ui.labelDim(area.removeFromTop(15.0f), "selection");

        if (ui.button(ui.id("ctx.delete"), area.removeFromTop(t.rowHeight), "delete",
                      Ui::ButtonStyle::Ghost)) {
            deleteSelection();
            ui.closePopup();
        }
        if (ui.button(ui.id("ctx.duplicate"), area.removeFromTop(t.rowHeight), "duplicate",
                      Ui::ButtonStyle::Ghost)) {
            duplicateSelection();
            ui.closePopup();
        }
        if (ui.button(ui.id("ctx.bypass"), area.removeFromTop(t.rowHeight), "bypass",
                      Ui::ButtonStyle::Ghost)) {
            bypassSelection();
            ui.closePopup();
        }
        ui.separator(area.removeFromTop(7.0f));
    }

    ui.labelDim(area.removeFromTop(15.0f), "add node");

    std::string lastGroup;
    for (const NodeTypeInfo& type : NodeFactory::instance().types()) {
        if (area.height < t.rowHeight) break;

        if (type.paletteGroup != lastGroup) {
            lastGroup = type.paletteGroup;
            if (area.height > t.rowHeight * 2.0f)
                ui.labelDim(area.removeFromTop(14.0f), lastGroup);
        }

        const Rect row = area.removeFromTop(t.rowHeight);
        if (ui.button(ui.id("ctx.add." + type.typeName), row, type.displayName,
                      Ui::ButtonStyle::Ghost)) {
            addNodeAt(type.typeName, contextMenuWorld_);
            ui.closePopup();
        }
        if (ui.isHot(ui.id("ctx.add." + type.typeName)))
            ui.setTooltip(type.description);
    }

    ui.endPopup();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void PatcherView::render(Ui& ui, const Rect& bounds) {
    if (!engine_) return;

    ui.pushClip(bounds);

    drawGrid(ui, bounds);
    drawCables(ui, bounds);

    // Only the node on top of the pointer takes input. The others are drawn
    // exactly as before and ignore the mouse, because a widget belonging to a
    // node that is *underneath* another one is not the thing being clicked -
    // and until this was here, it was: the node drawn first won every press its
    // rectangle covered, whatever was drawn over it. Two nodes overlapping is
    // ordinary, so this presented as controls that stopped responding once
    // something was dragged across them, and as drops landing on the wrong
    // node, both of which change with the zoom.
    const NodeId interactive = topmostNodeAt(ui.input().mousePosition, bounds);

    // Selected nodes draw last so their glow is not painted over.
    for (const auto& node : engine_->graph().nodes())
        if (!isSelected(node->id())) drawNode(ui, *node, bounds, node->id() == interactive);
    for (const auto& node : engine_->graph().nodes())
        if (isSelected(node->id())) drawNode(ui, *node, bounds, node->id() == interactive);

    handleInput(ui, bounds);
    handleShortcuts(ui, bounds);

    // A file dragged from the browser gets a landing zone.
    if (ui.dragging() && ui.dragType() == "file" && ui.hovering(bounds)) {
        ui.draw().addRect(bounds.deflated(3.0f), theme().accent.withAlpha(0.5f), 2.0f,
                          theme().cornerRadiusLarge);
    }

    if (ui.acceptDrop(bounds, "file")) {
        handleFileDrop(ui.dragPayload(), ui.input().mousePosition, bounds);
    }

    ui.popClip();

    // The menus are drawn outside the clip so they can overhang the canvas.
    handleContextMenu(ui, bounds);
    handlePortMenu(ui, bounds);
}

} // namespace acm::ui

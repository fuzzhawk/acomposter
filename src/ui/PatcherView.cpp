#include "PatcherView.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"
#include "../nodes/CrossfaderNode.h"
#include "../nodes/LooperNode.h"
#include "../nodes/MixerNode.h"
#include "../nodes/NodeFactory.h"
#include "../nodes/BuildNode.h"
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

Vec2 PatcherView::screenToWorld(Vec2 screen, const Rect& bounds) const {
    return { (screen.x - bounds.left()) / zoom_ - pan_.x,
             (screen.y - bounds.top()) / zoom_ - pan_.y };
}

Vec2 PatcherView::worldToScreen(Vec2 world, const Rect& bounds) const {
    return { (world.x + pan_.x) * zoom_ + bounds.left(),
             (world.y + pan_.y) * zoom_ + bounds.top() };
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

    zoom_ = clampValue(std::min((bounds.width - margin) / contentWidth,
                                (bounds.height - margin) / contentHeight),
                       kMinZoom, 1.0f);

    // Centre the content in the view.
    pan_ = { (bounds.width / zoom_ - contentWidth) * 0.5f - minX,
             (bounds.height / zoom_ - contentHeight) * 0.5f - minY };
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

float PatcherView::nodeWidth(const Node& node) const {
    if (node.canvasWidth > 0.0f) return node.canvasWidth;

    if (node.typeName() == "sample.player" || node.typeName() == "looper") return 230.0f;
    // Wide enough for a row of section buttons and a column of stem strips.
    if (node.typeName() == "stem.player") return 340.0f;
    if (node.typeName() == "color") return 260.0f;
    if (node.typeName() == "build") return 220.0f;
    if (node.typeName().rfind("mixer.", 0) == 0) {
        const auto* mixer = static_cast<const MixerNode*>(&node);
        return clampValue(46.0f * static_cast<float>(mixer->channelCount()) + 20.0f, 180.0f, 780.0f);
    }
    if (node.typeName() == vst2::VstNode::kTypeName) return 220.0f;
    return theme().nodeMinWidth;
}

float PatcherView::nodeHeight(const Node& node) const {
    const Theme& t = theme();
    if (node.collapsed) return t.nodeHeaderHeight;

    // Tall enough for the ports, whatever the body wants.
    const float portHeight = static_cast<float>(std::max(node.numInputs(), node.numOutputs()))
                           * kPortSpacing;

    float bodyHeight = 64.0f;
    const std::string& type = node.typeName();

    if (type == "sample.player") bodyHeight = 128.0f;
    else if (type == "stem.player") bodyHeight = 292.0f;
    else if (type == "color") bodyHeight = 132.0f;
    else if (type == "build") bodyHeight = 116.0f;
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

    return t.nodeHeaderHeight + std::max(bodyHeight, portHeight + 8.0f);
}

Rect PatcherView::nodeBounds(const Node& node, const Rect& viewBounds) const {
    const Vec2 topLeft = worldToScreen({ node.canvasX, node.canvasY }, viewBounds);
    return Rect{ topLeft.x, topLeft.y, nodeWidth(node) * zoom_, nodeHeight(node) * zoom_ };
}

Vec2 PatcherView::portPosition(const Node& node, PortIndex port, bool isInput,
                               const Rect& viewBounds) const {
    const Rect bounds = nodeBounds(node, viewBounds);
    const Theme& t = theme();

    const int count = isInput ? node.numInputs() : node.numOutputs();
    if (count <= 0) return bounds.centre();

    // Ports are distributed down the body, below the header.
    const float top = bounds.top() + t.nodeHeaderHeight * zoom_;
    const float available = std::max(bounds.height - t.nodeHeaderHeight * zoom_, 1.0f);
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

PatcherView::PortHit PatcherView::hitTestPorts(Ui& ui, const Rect& viewBounds) const {
    if (!engine_) return {};

    const Vec2 pointer = ui.input().mousePosition;
    // A generous radius: ports are small, and missing one mid-performance is
    // more annoying than occasionally grabbing the wrong one.
    const float radius = std::max(9.0f, theme().portRadius * zoom_ * 1.8f);

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

bool PatcherView::handleFileDrop(const std::string& utf8Path, Vec2 screenPosition,
                                 const Rect& bounds) {
    if (!engine_ || !audiofile::isSupportedFile(utf8Path)) return false;

    // Dropping onto an existing sample player replaces its file; dropping on
    // empty canvas makes a new one. Both are what people try first.
    const Vec2 world = screenToWorld(screenPosition, bounds);

    for (const auto& node : engine_->graph().nodes()) {
        const Rect box = nodeBounds(*node, bounds);
        if (!box.contains(screenPosition)) continue;

        if (node->typeName() == "sample.player") {
            auto* player = static_cast<SamplePlayerNode*>(node.get());
            return player->loadFile(utf8Path, nullptr);
        }

        // A stem player has eight slots stacked down its body, so which one was
        // hit has to come from where in the node the pointer landed. The strips
        // are the bottom of the body, laid out by drawStemPlayerBody.
        if (node->typeName() == "stem.player") {
            auto* player = static_cast<StemPlayerNode*>(node.get());

            const float scale = zoom_;
            const float bodyTop = box.top() + theme().nodeHeaderHeight * scale;
            // Section grid, launch row, tempo row and the gaps above the
            // strips. Kept in one expression so it is obvious this has to move
            // whenever drawStemPlayerBody's layout does.
            const float stripsTop = bodyTop + (76.0f + 4.0f + 18.0f + 3.0f + 18.0f + 4.0f) * scale;
            const float stripHeight = 17.0f * scale;

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
            auto* build = static_cast<BuildNode*>(node.get());
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
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(bounds, t.canvas);

    const float spacing = kGridSpacing * zoom_;
    if (spacing < 6.0f) return;   // too dense to be anything but noise

    // Every fourth line is brighter, which gives the eye something to measure
    // distance against when the canvas is otherwise empty.
    const float originX = std::fmod(pan_.x * zoom_, spacing * 4.0f);
    const float originY = std::fmod(pan_.y * zoom_, spacing * 4.0f);

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
        float thickness = t.cableThickness * zoom_;

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
            list.addCircleFilled(midpoint, 4.0f * zoom_, t.canvas);
            list.addCircle(midpoint, 4.0f * zoom_, t.cableFeedback, 1.5f);
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
                           t.cableThickness * zoom_ * 1.2f);
            list.addCircleFilled(to, 4.0f, t.accent);
        }
    }
}

void PatcherView::drawNode(Ui& ui, Node& node, const Rect& bounds) {
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

    const Rect header{ rect.left(), rect.top(), rect.width, t.nodeHeaderHeight * zoom_ };
    list.addRectFilledGradient(header, accent.withAlpha(bypassed ? 0.12f : 0.30f),
                               accent.withAlpha(bypassed ? 0.04f : 0.10f),
                               t.cornerRadiusLarge, gfx::Corners::Top);
    // A solid stripe down the left edge is the fastest category read at a glance.
    list.addRectFilled(Rect{ rect.left(), rect.top(), 3.0f * zoom_, rect.height },
                       accent.withAlpha(bypassed ? 0.3f : 0.9f),
                       t.cornerRadiusLarge, gfx::Corners::Left);

    list.addRect(rect, selected ? accent : (hovered ? t.borderStrong : t.border),
                 selected ? 1.6f : t.borderWidth, t.cornerRadiusLarge);

    // -- header content ----------------------------------------------------
    Rect headerContent = header.deflated(6.0f * zoom_);
    headerContent.removeFromLeft(4.0f * zoom_);

    const Rect collapseArea = headerContent.removeFromRight(16.0f * zoom_);
    const Rect bypassArea = headerContent.removeFromRight(18.0f * zoom_);

    if (zoom_ > 0.55f) {
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
    Rect body{ rect.left() + 6.0f * zoom_, header.bottom() + 2.0f * zoom_,
               rect.width - 12.0f * zoom_,
               rect.bottom() - header.bottom() - 6.0f * zoom_ };

    if (!node.errorText().empty()) {
        const Rect errorRow = body.removeFromBottom(16.0f * zoom_);
        list.addRectFilled(errorRow, t.danger.withAlpha(0.12f), 2.0f);
        list.addTextClipped(ui.font(t.fontSmall), errorRow.deflated(3.0f), t.danger,
                            node.errorText());
        if (ui.hovering(errorRow)) ui.setTooltip(node.errorText());
    }

    // Below this the controls are too small to hit reliably, so the node shows
    // only its identity and its meters.
    if (zoom_ > 0.62f && body.height > 12.0f) {
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

        const float radius = t.portRadius * zoom_;
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

        const float radius = t.portRadius * zoom_;
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

    // Dropping a file straight onto the strip is the obvious gesture.
    if (ui.acceptDrop(waveform, "file")) {
        std::string error;
        player.loadFile(ui.dragPayload(), &error);
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

    Rect sectionArea = area.removeFromTop(s * 76.0f);
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

                Colour fill = t.widgetBackground;
                if (isActive) fill = t.accent.withAlpha(0.30f);
                else if (isPending) fill = t.control.withAlpha(0.28f);

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
                                             cell.width * progress, 3.0f }, t.accent);
                    list.addRect(cell, t.accent, 1.0f, t.cornerRadius);
                } else if (isPending) {
                    list.addRect(cell, t.control, 1.0f, t.cornerRadius);
                }

                list.addTextClipped(ui.font(t.fontSmall), cell.deflated(s * 3.0f),
                                    isActive ? t.text : t.textDim, section.name,
                                    DrawList::Align::Centre);
            }
        }
    }

    area.removeFromTop(s * 4.0f);

    // -- launch and divide -------------------------------------------------
    Rect controlRow = area.removeFromTop(s * 18.0f);
    ui.parameterChoice(controlRow.removeFromLeft(controlRow.width * 0.55f),
                       node.parameter(node.indexOfParameter("launch")));
    controlRow.removeFromLeft(s * 4.0f);
    ui.parameterChoice(controlRow, node.parameter(node.indexOfParameter("divide")));

    area.removeFromTop(s * 3.0f);

    // -- tempo -------------------------------------------------------------
    Rect tempoRow = area.removeFromTop(s * 18.0f);

    const double stemBpm = stems.stemBpm();
    const double transportBpm = engine_ ? engine_->transport().bpm() : 120.0;
    const double effective = stemBpm > 0.0 ? stemBpm : transportBpm;

    char tempoText[64];
    std::snprintf(tempoText, sizeof(tempoText), "%.2f bpm", effective);
    list.addTextClipped(ui.font(t.fontSmall), tempoRow.removeFromLeft(tempoRow.width * 0.34f),
                        stemBpm > 0.0 ? t.text : t.textFaint, tempoText);

    if (ui.button(ui.idFrom(&node, 420), tempoRow.removeFromLeft(tempoRow.width * 0.42f),
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

    tempoRow.removeFromLeft(s * 3.0f);

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

    area.removeFromTop(s * 4.0f);

    // -- stem strips -------------------------------------------------------
    for (int slot = 0; slot < kMaxStems; ++slot) {
        if (area.height < s * 18.0f) break;
        Rect row = area.removeFromTop(s * 17.0f);

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
        list.addRect(strip, t.border.withAlpha(0.5f), 1.0f, 2.0f);
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
    Rect switchArea = area.removeFromTop(s * 52.0f).deflated(s * 2.0f);

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
        list.addRectFilled(Rect{ switchArea.left(), switchArea.bottom() - 4.0f,
                                 switchArea.width * progress, 4.0f }, t.danger);
        list.addGlow(switchArea, t.danger.withAlpha(0.30f), 8.0f, t.cornerRadius, 4);
    }
    list.addRect(switchArea, running ? t.danger : t.border, 1.0f, t.cornerRadius);

    list.addTextClipped(ui.font(t.fontUiBold), switchArea,
                        running ? t.text : t.textDim,
                        running ? "BUILDING" : "hold to build", DrawList::Align::Centre);

    if (hovered) ui.setTooltip("Hold. Releases on the next bar so the drop lands in time.");

    area.removeFromTop(s * 4.0f);

    // -- shape -------------------------------------------------------------
    Rect row = area.removeFromTop(s * 17.0f);
    ui.parameterSlider(row, node.parameter(node.indexOfParameter("bars")), t.control);

    area.removeFromTop(s * 3.0f);
    if (area.height >= s * 17.0f) {
        Rect curveRow = area.removeFromTop(s * 17.0f);
        ui.parameterChoice(curveRow.removeFromLeft(curveRow.width * 0.5f - 2.0f),
                           node.parameter(node.indexOfParameter("curve")));
        curveRow.removeFromLeft(s * 4.0f);
        ui.parameterChoice(curveRow, node.parameter(node.indexOfParameter("release")));
    }

    // A build with nothing wired to it is silent and confusing, so say so.
    if (build.stemPlayer() == kInvalidNode && build.colorNode() == kInvalidNode
        && area.height >= s * 14.0f) {
        list.addTextClipped(ui.font(t.fontSmall), area.removeFromTop(s * 14.0f), t.textFaint,
                            "not wired - pick targets in the inspector",
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
            pan_ += input.mouseDelta / zoom_;
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
                                               theme().nodeMinWidth, 900.0f);
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

void PatcherView::handleContextMenu(Ui& ui, const Rect& bounds) {
    const InputState& input = ui.input();
    const UiId menuId = ui.id("patcher.context");

    if (ui.hovering(bounds) && input.mousePressed[static_cast<int>(MouseButton::Right)]
        && !ui.popupOpen(menuId)) {
        contextMenuWorld_ = screenToWorld(input.mousePosition, bounds);

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

    // Selected nodes draw last so their glow is not painted over.
    for (const auto& node : engine_->graph().nodes())
        if (!isSelected(node->id())) drawNode(ui, *node, bounds);
    for (const auto& node : engine_->graph().nodes())
        if (isSelected(node->id())) drawNode(ui, *node, bounds);

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

    // The menu is drawn outside the clip so it can overhang the canvas.
    handleContextMenu(ui, bounds);
}

} // namespace acm::ui

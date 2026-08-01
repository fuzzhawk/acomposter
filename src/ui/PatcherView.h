// The patcher canvas: the surface the whole application is built around.
//
// Nodes are laid out in world coordinates stored on the nodes themselves, so
// the canvas is a pure view onto the graph rather than a parallel model that
// could drift out of sync. Every frame it re-derives everything it draws from
// the graph, which means a node added by the patch loader, by a drag from the
// browser, or by the context menu all appear the same way with no extra code.
#pragma once

#include "../core/Engine.h"
#include "../meta/Metasurface.h"
#include "Ui.h"

#include <string>
#include <vector>

namespace acm::vst2 { class PluginManager; struct PluginDescription; }

namespace acm::ui {

// What the canvas is currently doing with the pointer.
enum class CanvasMode {
    Idle,
    PanningCanvas,
    DraggingNodes,
    DraggingCable,
    Marquee,
    ResizingNode,
};

class PatcherView {
public:
    void initialise(Engine* engine, Metasurface* metasurface, vst2::PluginManager* plugins);

    // Draws and handles the canvas within `bounds`.
    void render(Ui& ui, const Rect& bounds);

    // -- view transform ----------------------------------------------------
    Vec2 pan() const noexcept { return pan_; }
    void setPan(Vec2 p) noexcept { pan_ = p; }
    float zoom() const noexcept { return zoom_; }
    void setZoom(float z) noexcept { zoom_ = clampValue(z, kMinZoom, kMaxZoom); }
    void frameAll(const Rect& bounds);
    void resetView();

    // -- selection ---------------------------------------------------------
    const std::vector<NodeId>& selection() const noexcept { return selection_; }
    void select(NodeId node, bool additive);
    void selectAll();
    void clearSelection();
    bool isSelected(NodeId node) const;

    // -- editing -----------------------------------------------------------
    // Creates a node of `typeName` at a world position, wired into the
    // selection where that makes sense.
    NodeId addNodeAt(const std::string& typeName, Vec2 worldPosition);
    // Places an already-built node (used for plugins, which the factory cannot
    // construct without a description).
    NodeId placeNode(std::unique_ptr<Node> node, Vec2 worldPosition);
    void deleteSelection();
    void duplicateSelection();
    void bypassSelection();

    // -- stem effect chains ------------------------------------------------
    // Adds a plugin to the end of the chain hanging off one of a stem player's
    // outputs, wiring it in so the performer never has to. Returns the new node,
    // or kInvalidNode.
    //
    // The rack is made of ordinary nodes wired in series rather than plugin
    // instances hidden inside the stem player. That is a deliberate choice: it
    // reuses the whole VST host - editors, bridging, state, the scanner - and it
    // means the colour engine's existing addressing reaches these plugins with
    // no changes at all. The cost is that they are visible on the canvas, which
    // for a patcher is arguably where they belong.
    NodeId addToStemChain(NodeId stemPlayer, int stemSlot,
                          const vst2::PluginDescription& description, bool forceBridge);
    // Removes a node from a chain and closes the gap so the audio still flows.
    bool removeFromChain(NodeId node);
    // Duplicates one stem's rack onto another, plugin state and all. Anything
    // already on the destination is left in place and the copy is appended, so
    // this adds rather than silently replaces. Returns how many were copied.
    int copyStemChain(NodeId stemPlayer, int fromSlot, int toSlot);

    // Lays a stem player's racks out in tidy rows to the right of it.
    void tidyStemChains(NodeId stemPlayer);

    // Loads a sample into the player under the pointer, or makes a new player.
    // Used by both the file browser and Windows drag-and-drop.
    bool handleFileDrop(const std::string& utf8Path, Vec2 screenPosition, const Rect& bounds);

    // The node the pointer is over, for the inspector.
    NodeId hoveredNode() const noexcept { return hovered_; }
    NodeId focusedNode() const;

    // Set when a node's editor button is clicked; the application acts on it.
    NodeId consumeEditorRequest();

    Vec2 screenToWorld(Vec2 screen, const Rect& bounds) const;
    Vec2 worldToScreen(Vec2 world, const Rect& bounds) const;

    // Where a newly created node should land: the middle of the visible area,
    // nudged so successive additions do not stack exactly on top of each other.
    Vec2 defaultDropPosition(const Rect& bounds);

private:
    struct PortHit {
        NodeId node = kInvalidNode;
        PortIndex port = 0;
        bool isInput = false;
        bool valid() const { return node != kInvalidNode; }
    };

    // Node bodies are authored in unscaled units and multiplied by this, so a
    // row keeps its share of the node at every zoom. Without it the layout was
    // fixed-size inside a rect that was not: zoomed in it huddled at the top,
    // zoomed out it overflowed and the last controls fell off the bottom - and
    // since wheel zoom is multiplicative it almost never lands back on exactly
    // 1.0, so zooming out and back left the node subtly wrong.
    float bodyScale() const noexcept { return zoom_; }

    Rect nodeBounds(const Node& node, const Rect& viewBounds) const;
    float nodeHeight(const Node& node) const;
    float nodeWidth(const Node& node) const;

    Vec2 portPosition(const Node& node, PortIndex port, bool isInput, const Rect& viewBounds) const;
    PortHit hitTestPorts(Ui& ui, const Rect& viewBounds) const;
    NodeId hitTestNodes(Ui& ui, const Rect& viewBounds) const;
    ConnectionId hitTestCables(Ui& ui, const Rect& viewBounds) const;

    void drawGrid(Ui& ui, const Rect& bounds) const;
    void drawCables(Ui& ui, const Rect& bounds);
    void drawNode(Ui& ui, Node& node, const Rect& bounds);
    void drawNodeBody(Ui& ui, Node& node, const Rect& body);
    void drawSamplePlayerBody(Ui& ui, Node& node, const Rect& body);
    void drawStemPlayerBody(Ui& ui, Node& node, const Rect& body);
    void drawColorBody(Ui& ui, Node& node, const Rect& body);
    void drawBuildBody(Ui& ui, Node& node, const Rect& body);
    void drawLooperBody(Ui& ui, Node& node, const Rect& body);
    void drawMixerBody(Ui& ui, Node& node, const Rect& body);
    void drawCrossfaderBody(Ui& ui, Node& node, const Rect& body);
    void drawPluginBody(Ui& ui, Node& node, const Rect& body);
    void drawGenericBody(Ui& ui, Node& node, const Rect& body);

    void handleInput(Ui& ui, const Rect& bounds);
    void handleContextMenu(Ui& ui, const Rect& bounds);
    void handleShortcuts(Ui& ui, const Rect& bounds);

    static constexpr float kMinZoom = 0.3f;
    static constexpr float kMaxZoom = 2.5f;

    Engine* engine_ = nullptr;
    Metasurface* metasurface_ = nullptr;
    vst2::PluginManager* plugins_ = nullptr;

    Vec2 pan_{ 0.0f, 0.0f };
    float zoom_ = 1.0f;

    CanvasMode mode_ = CanvasMode::Idle;
    std::vector<NodeId> selection_;
    NodeId hovered_ = kInvalidNode;
    ConnectionId hoveredCable_ = kInvalidConnection;

    // Drag state.
    Vec2 dragStartWorld_{ 0.0f, 0.0f };
    Vec2 marqueeStart_{ 0.0f, 0.0f };
    std::vector<std::pair<NodeId, Vec2>> dragOrigins_;
    PortHit cableSource_;
    NodeId resizingNode_ = kInvalidNode;
    float resizeStartWidth_ = 0.0f;

    NodeId editorRequest_ = kInvalidNode;
    NodeId renamingNode_ = kInvalidNode;
    std::string renameBuffer_;

    // Where the context menu was opened, in world space.
    Vec2 contextMenuWorld_{ 0.0f, 0.0f };
    int paletteScrollIndex_ = 0;
    int newNodeCounter_ = 0;
};

} // namespace acm::ui

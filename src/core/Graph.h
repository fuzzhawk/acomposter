// The patch: nodes, connections, and the render schedule compiled from them.
//
// Threading
// ---------
// Structural edits (add/remove node, connect/disconnect) happen on the message
// thread and never touch the audio thread's data. Instead each edit compiles a
// fresh immutable RenderSchedule and publishes it through an AtomicResource, so
// the audio thread picks up the new topology at a block boundary without a lock
// and without a dropout. Nodes removed from the graph are retired for a couple
// of blocks before their memory is released.
//
// Feedback
// --------
// Cycles are legal. The schedule detects back edges during the topological sort
// and routes them through dedicated one-block delay buffers, so a looper feeding
// its own input resolves to a single block of latency instead of a deadlock.
#pragma once

#include "AtomicResource.h"
#include "AudioBuffer.h"
#include "Node.h"
#include "Transport.h"
#include "Types.h"

#include <memory>
#include <string>
#include <vector>

namespace acm {

struct Connection {
    ConnectionId id = kInvalidConnection;
    NodeId sourceNode = kInvalidNode;
    PortIndex sourcePort = 0;
    NodeId destNode = kInvalidNode;
    PortIndex destPort = 0;
};

// ---------------------------------------------------------------------------
// Compiled schedule - read-only once published
// ---------------------------------------------------------------------------

class RenderSchedule {
public:
    // How one port of one node is wired for the block.
    struct BusPlan {
        int bufferIndex = -1;         // buffer presented to the node
        int channels = 2;
        bool connected = false;

        // When a port has several sources, or one source whose channel count
        // differs from the port's, the graph mixes into `bufferIndex` first.
        bool needsMix = false;
        std::vector<int> sourceBuffers;
    };

    struct Step {
        Node* node = nullptr;
        std::vector<BusPlan> inputs;
        std::vector<BusPlan> outputs;
    };

    // Copied after the last step so the destination reads it next block.
    struct FeedbackTap {
        int fromBuffer = -1;
        int toBuffer = -1;
    };

    std::vector<Step> steps;
    std::vector<FeedbackTap> feedbackTaps;
    std::vector<std::unique_ptr<AudioBuffer>> buffers;
    int silenceBuffer = -1;

    // Diagnostics surfaced in the UI.
    int feedbackEdgeCount = 0;
    std::vector<NodeId> order;

    AudioBuffer& buffer(int i) noexcept { return *buffers[static_cast<std::size_t>(i)]; }
    const AudioBuffer& buffer(int i) const noexcept { return *buffers[static_cast<std::size_t>(i)]; }
};

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------

class Graph {
public:
    Graph();
    ~Graph();

    // The engine's block counter, used to decide when retired objects are safe
    // to free. Must be set before the first edit.
    void setClock(const BlockCounter* clock);

    // -- structure (message thread) ----------------------------------------
    NodeId addNode(std::unique_ptr<Node> node);
    bool removeNode(NodeId id);
    void clear();

    Node* node(NodeId id) noexcept;
    const Node* node(NodeId id) const noexcept;
    std::size_t nodeCount() const noexcept { return nodes_.size(); }
    // Iteration order is creation order, which keeps saved patches stable.
    const std::vector<std::unique_ptr<Node>>& nodes() const noexcept { return nodes_; }

    // Returns kInvalidConnection when the connection is illegal or duplicated.
    ConnectionId connect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort);
    bool disconnect(ConnectionId id);
    bool disconnect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort);
    void disconnectNode(NodeId id);
    // Removes every connection arriving at one input port.
    void disconnectInput(NodeId dst, PortIndex dstPort);

    bool canConnect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort,
                    std::string* reason = nullptr) const;
    bool isConnected(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort) const;

    const std::vector<Connection>& connections() const noexcept { return connections_; }
    const Connection* connection(ConnectionId id) const noexcept;

    // True when the edge had to be scheduled as a one-block feedback tap.
    bool isFeedbackEdge(ConnectionId id) const;

    // -- preparation -------------------------------------------------------
    void prepare(double sampleRate, int maxBlockSize);
    void reset();
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlockSize() const noexcept { return maxBlockSize_; }

    // -- rendering (audio thread) ------------------------------------------
    void render(const TransportState& transport, int frames, std::uint64_t streamFrame,
                const AudioBuffer* deviceInput, AudioBuffer* deviceOutput);

    // -- maintenance (message thread) --------------------------------------
    // Frees retired nodes and schedules. Call once per UI frame.
    void collectGarbage();

    // Forces a recompile; edits do this automatically.
    void rebuildSchedule();

    int scheduledFeedbackEdges() const noexcept { return feedbackEdgeCount_; }

    // Id of the next node that will be created. Used by the patch loader to keep
    // ids stable across a save/load round trip.
    void setNextNodeId(NodeId next) noexcept { nextNodeId_ = next; }
    NodeId peekNextNodeId() const noexcept { return nextNodeId_; }

private:
    struct RetiredNode {
        std::unique_ptr<Node> node;
        std::uint64_t block = 0;
    };

    // Topological order ignoring back edges; fills backEdges_ as a side effect.
    std::vector<NodeId> computeOrder(std::vector<ConnectionId>& backEdges) const;

    int allocateBuffer(RenderSchedule& s, int channels) const;

    std::uint64_t now() const noexcept;

    std::vector<std::unique_ptr<Node>> nodes_;
    std::vector<Connection> connections_;
    std::vector<RetiredNode> retiredNodes_;
    std::vector<ConnectionId> feedbackEdges_;

    AtomicResource<RenderSchedule> schedule_;
    const BlockCounter* clock_ = nullptr;

    NodeId nextNodeId_ = 1;
    ConnectionId nextConnectionId_ = 1;
    int feedbackEdgeCount_ = 0;

    double sampleRate_ = 48000.0;
    int maxBlockSize_ = 512;
    bool prepared_ = false;
};

} // namespace acm

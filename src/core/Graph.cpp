#include "Graph.h"

#include "Denormals.h"

#include <algorithm>
#include <cstring>

namespace acm {
namespace {

// Copies `src` into `dst` adapting the channel count.
//
// Upmix repeats source channels round-robin (mono to stereo lands in both
// sides); downmix averages the extras in so nothing is silently dropped.
void mixInto(AudioBuffer& dst, const AudioBuffer& src, int frames, bool accumulate) {
    const int dstCh = dst.channels();
    const int srcCh = src.channels();
    if (dstCh <= 0 || srcCh <= 0) return;

    const int n = std::min({ frames, dst.frames(), src.frames() });

    if (srcCh >= dstCh) {
        // Fold the surplus source channels down onto the destination.
        for (int c = 0; c < dstCh; ++c) {
            float* d = dst.channel(c);
            if (!accumulate) std::memset(d, 0, sizeof(float) * static_cast<std::size_t>(n));
        }
        for (int sc = 0; sc < srcCh; ++sc) {
            const int dc = sc % dstCh;
            float* d = dst.channel(dc);
            const float* s = src.channel(sc);
            for (int i = 0; i < n; ++i) d[i] += s[i];
        }
        // Normalise the fold so a stereo-to-mono sum does not gain up by 6 dB.
        if (srcCh > dstCh) {
            const int foldsPerChannel = (srcCh + dstCh - 1) / dstCh;
            if (foldsPerChannel > 1) {
                const float scale = 1.0f / static_cast<float>(foldsPerChannel);
                for (int c = 0; c < dstCh; ++c) {
                    float* d = dst.channel(c);
                    for (int i = 0; i < n; ++i) d[i] *= scale;
                }
            }
        }
        return;
    }

    // Upmix: repeat the source channels.
    for (int c = 0; c < dstCh; ++c) {
        float* d = dst.channel(c);
        const float* s = src.channel(c % srcCh);
        if (accumulate) {
            for (int i = 0; i < n; ++i) d[i] += s[i];
        } else {
            std::memcpy(d, s, sizeof(float) * static_cast<std::size_t>(n));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------

Graph::Graph() = default;
Graph::~Graph() = default;

void Graph::setClock(const BlockCounter* clock) {
    clock_ = clock;
    schedule_.setClock(clock);
}

std::uint64_t Graph::now() const noexcept {
    return clock_ ? clock_->load(std::memory_order_acquire) : 0;
}

// ---------------------------------------------------------------------------
// Structure
// ---------------------------------------------------------------------------

NodeId Graph::addNode(std::unique_ptr<Node> node) {
    if (!node) return kInvalidNode;
    if (nodes_.size() >= static_cast<std::size_t>(kMaxNodes)) return kInvalidNode;

    const NodeId id = nextNodeId_++;
    node->setId(id);
    node->setOwningGraph(this);

    if (prepared_) {
        PrepareInfo info{ sampleRate_, maxBlockSize_, clock_ };
        node->prepare(info);
    }

    nodes_.push_back(std::move(node));
    rebuildSchedule();
    return id;
}

NodeId Graph::addNodeWithId(std::unique_ptr<Node> node, NodeId id) {
    if (!node || id == kInvalidNode) return kInvalidNode;
    if (this->node(id) != nullptr) return kInvalidNode;
    if (nodes_.size() >= static_cast<std::size_t>(kMaxNodes)) return kInvalidNode;

    node->setId(id);
    node->setOwningGraph(this);

    if (prepared_) {
        PrepareInfo info{ sampleRate_, maxBlockSize_, clock_ };
        node->prepare(info);
    }

    nodes_.push_back(std::move(node));
    if (id >= nextNodeId_) nextNodeId_ = id + 1;

    rebuildSchedule();
    return id;
}

bool Graph::removeNode(NodeId id) {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(),
                                 [id](const std::unique_ptr<Node>& n) { return n->id() == id; });
    if (it == nodes_.end()) return false;

    disconnectNode(id);

    RetiredNode retired;
    retired.node = std::move(*it);
    retired.block = now();
    nodes_.erase(it);

    // Publish a schedule that no longer references the node *before* letting go
    // of it; collectGarbage() frees it once the audio thread has moved on.
    rebuildSchedule();
    retiredNodes_.push_back(std::move(retired));
    return true;
}

void Graph::clear() {
    connections_.clear();
    for (auto& n : nodes_)
        retiredNodes_.push_back(RetiredNode{ std::move(n), now() });
    nodes_.clear();
    nextNodeId_ = 1;
    nextConnectionId_ = 1;
    rebuildSchedule();
}

Node* Graph::node(NodeId id) noexcept {
    for (auto& n : nodes_)
        if (n->id() == id) return n.get();
    return nullptr;
}

const Node* Graph::node(NodeId id) const noexcept {
    for (const auto& n : nodes_)
        if (n->id() == id) return n.get();
    return nullptr;
}

bool Graph::canConnect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort,
                       std::string* reason) const {
    const auto fail = [reason](const char* why) {
        if (reason) *reason = why;
        return false;
    };

    if (src == kInvalidNode || dst == kInvalidNode) return fail("no such node");
    if (src == dst) return fail("a node cannot patch into itself");

    const Node* s = node(src);
    const Node* d = node(dst);
    if (!s) return fail("source node no longer exists");
    if (!d) return fail("destination node no longer exists");
    if (srcPort < 0 || srcPort >= s->numOutputs()) return fail("no such output");
    if (dstPort < 0 || dstPort >= d->numInputs()) return fail("no such input");
    if (isConnected(src, srcPort, dst, dstPort)) return fail("already patched");

    if (reason) reason->clear();
    return true;
}

bool Graph::isConnected(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort) const {
    for (const auto& c : connections_)
        if (c.sourceNode == src && c.sourcePort == srcPort
            && c.destNode == dst && c.destPort == dstPort)
            return true;
    return false;
}

ConnectionId Graph::connect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort) {
    if (!canConnect(src, srcPort, dst, dstPort)) return kInvalidConnection;

    Connection c;
    c.id = nextConnectionId_++;
    c.sourceNode = src;
    c.sourcePort = srcPort;
    c.destNode = dst;
    c.destPort = dstPort;
    connections_.push_back(c);
    rebuildSchedule();
    return c.id;
}

bool Graph::disconnect(ConnectionId id) {
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [id](const Connection& c) { return c.id == id; });
    if (it == connections_.end()) return false;
    connections_.erase(it);
    rebuildSchedule();
    return true;
}

bool Graph::disconnect(NodeId src, PortIndex srcPort, NodeId dst, PortIndex dstPort) {
    const auto it = std::find_if(connections_.begin(), connections_.end(),
                                 [&](const Connection& c) {
                                     return c.sourceNode == src && c.sourcePort == srcPort
                                         && c.destNode == dst && c.destPort == dstPort;
                                 });
    if (it == connections_.end()) return false;
    connections_.erase(it);
    rebuildSchedule();
    return true;
}

void Graph::disconnectNode(NodeId id) {
    const auto before = connections_.size();
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [id](const Connection& c) {
                                          return c.sourceNode == id || c.destNode == id;
                                      }),
                       connections_.end());
    if (connections_.size() != before) rebuildSchedule();
}

void Graph::disconnectInput(NodeId dst, PortIndex dstPort) {
    const auto before = connections_.size();
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [&](const Connection& c) {
                                          return c.destNode == dst && c.destPort == dstPort;
                                      }),
                       connections_.end());
    if (connections_.size() != before) rebuildSchedule();
}

const Connection* Graph::connection(ConnectionId id) const noexcept {
    for (const auto& c : connections_)
        if (c.id == id) return &c;
    return nullptr;
}

bool Graph::isFeedbackEdge(ConnectionId id) const {
    return std::find(feedbackEdges_.begin(), feedbackEdges_.end(), id) != feedbackEdges_.end();
}

// ---------------------------------------------------------------------------
// Ordering
// ---------------------------------------------------------------------------

std::vector<NodeId> Graph::computeOrder(std::vector<ConnectionId>& backEdges) const {
    backEdges.clear();

    // Map node ids to dense indices for the traversal.
    const std::size_t n = nodes_.size();
    std::vector<NodeId> ids(n);
    for (std::size_t i = 0; i < n; ++i) ids[i] = nodes_[i]->id();

    const auto indexOf = [&](NodeId id) -> int {
        for (std::size_t i = 0; i < n; ++i)
            if (ids[i] == id) return static_cast<int>(i);
        return -1;
    };

    struct Edge { int to; ConnectionId id; };
    std::vector<std::vector<Edge>> adjacency(n);
    for (const auto& c : connections_) {
        const int from = indexOf(c.sourceNode);
        const int to = indexOf(c.destNode);
        if (from >= 0 && to >= 0)
            adjacency[static_cast<std::size_t>(from)].push_back(Edge{ to, c.id });
    }

    // Iterative DFS. Grey nodes are on the current stack, so an edge into one is
    // a cycle: mark it as feedback and leave it out of the ordering constraints.
    enum class Colour : std::uint8_t { White, Grey, Black };
    std::vector<Colour> colour(n, Colour::White);
    std::vector<NodeId> reverseOrder;
    reverseOrder.reserve(n);

    struct Frame { int node; std::size_t next; };
    std::vector<Frame> stack;

    for (std::size_t root = 0; root < n; ++root) {
        if (colour[root] != Colour::White) continue;

        stack.push_back(Frame{ static_cast<int>(root), 0 });
        colour[root] = Colour::Grey;

        while (!stack.empty()) {
            Frame& f = stack.back();
            auto& edges = adjacency[static_cast<std::size_t>(f.node)];

            if (f.next < edges.size()) {
                const Edge e = edges[f.next++];
                const auto to = static_cast<std::size_t>(e.to);
                if (colour[to] == Colour::White) {
                    colour[to] = Colour::Grey;
                    stack.push_back(Frame{ e.to, 0 });
                } else if (colour[to] == Colour::Grey) {
                    backEdges.push_back(e.id);
                }
                continue;
            }

            colour[static_cast<std::size_t>(f.node)] = Colour::Black;
            reverseOrder.push_back(ids[static_cast<std::size_t>(f.node)]);
            stack.pop_back();
        }
    }

    std::reverse(reverseOrder.begin(), reverseOrder.end());
    return reverseOrder;
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

int Graph::allocateBuffer(RenderSchedule& s, int channels) const {
    s.buffers.push_back(std::make_unique<AudioBuffer>(clampValue(channels, 1, kMaxChannelsPerPort),
                                                      maxBlockSize_));
    return static_cast<int>(s.buffers.size()) - 1;
}

void Graph::rebuildSchedule() {
    auto schedule = std::make_shared<RenderSchedule>();

    if (!prepared_) {
        // Publish an empty schedule so the audio thread has something valid to
        // run before the device has opened.
        schedule_.publish(std::move(schedule));
        return;
    }

    std::vector<ConnectionId> backEdges;
    const std::vector<NodeId> order = computeOrder(backEdges);
    feedbackEdges_ = backEdges;
    feedbackEdgeCount_ = static_cast<int>(backEdges.size());

    schedule->order = order;
    schedule->feedbackEdgeCount = feedbackEdgeCount_;
    schedule->silenceBuffer = allocateBuffer(*schedule, kMaxChannelsPerPort);

    const auto isBackEdge = [&](ConnectionId id) {
        return std::find(backEdges.begin(), backEdges.end(), id) != backEdges.end();
    };

    // Every output port gets a dedicated buffer, keyed by (node, port).
    struct OutputSlot { NodeId node; PortIndex port; int buffer; };
    std::vector<OutputSlot> outputSlots;

    const auto findOutputBuffer = [&](NodeId nodeId, PortIndex port) -> int {
        for (const auto& slot : outputSlots)
            if (slot.node == nodeId && slot.port == port) return slot.buffer;
        return -1;
    };

    for (const auto& n : nodes_) {
        for (int p = 0; p < n->numOutputs(); ++p)
            outputSlots.push_back(OutputSlot{ n->id(), p, allocateBuffer(*schedule, n->outputPort(p).channels) });
    }

    // Back edges read from a delay buffer that is filled after the block ends.
    struct FeedbackSlot { ConnectionId edge; int buffer; };
    std::vector<FeedbackSlot> feedbackSlots;

    for (ConnectionId edge : backEdges) {
        const Connection* c = connection(edge);
        if (!c) continue;
        const Node* src = node(c->sourceNode);
        if (!src || c->sourcePort >= src->numOutputs()) continue;

        const int delayBuffer = allocateBuffer(*schedule, src->outputPort(c->sourcePort).channels);
        feedbackSlots.push_back(FeedbackSlot{ edge, delayBuffer });

        const int sourceBuffer = findOutputBuffer(c->sourceNode, c->sourcePort);
        if (sourceBuffer >= 0)
            schedule->feedbackTaps.push_back(RenderSchedule::FeedbackTap{ sourceBuffer, delayBuffer });
    }

    const auto feedbackBufferFor = [&](ConnectionId edge) -> int {
        for (const auto& slot : feedbackSlots)
            if (slot.edge == edge) return slot.buffer;
        return -1;
    };

    // Build one step per node, in topological order.
    schedule->steps.reserve(order.size());

    for (NodeId nodeId : order) {
        Node* n = node(nodeId);
        if (!n) continue;

        RenderSchedule::Step step;
        step.node = n;
        step.inputs.resize(static_cast<std::size_t>(n->numInputs()));
        step.outputs.resize(static_cast<std::size_t>(n->numOutputs()));

        for (int p = 0; p < n->numOutputs(); ++p) {
            auto& plan = step.outputs[static_cast<std::size_t>(p)];
            plan.bufferIndex = findOutputBuffer(nodeId, p);
            plan.channels = n->outputPort(p).channels;
            plan.connected = true;
        }

        for (int p = 0; p < n->numInputs(); ++p) {
            auto& plan = step.inputs[static_cast<std::size_t>(p)];
            plan.channels = n->inputPort(p).channels;

            // Gather every source arriving at this port.
            std::vector<int> sources;
            bool channelMismatch = false;

            for (const auto& c : connections_) {
                if (c.destNode != nodeId || c.destPort != p) continue;

                const int buffer = isBackEdge(c.id) ? feedbackBufferFor(c.id)
                                                    : findOutputBuffer(c.sourceNode, c.sourcePort);
                if (buffer < 0) continue;

                sources.push_back(buffer);
                if (schedule->buffer(buffer).channels() != plan.channels)
                    channelMismatch = true;
            }

            if (sources.empty()) {
                plan.bufferIndex = schedule->silenceBuffer;
                plan.connected = false;
                plan.needsMix = false;
                continue;
            }

            plan.connected = true;

            // A single source that already matches the port's layout can be
            // handed straight to the node with no copy at all.
            if (sources.size() == 1 && !channelMismatch) {
                plan.bufferIndex = sources[0];
                plan.needsMix = false;
                continue;
            }

            plan.needsMix = true;
            plan.sourceBuffers = std::move(sources);
            plan.bufferIndex = allocateBuffer(*schedule, plan.channels);
        }

        schedule->steps.push_back(std::move(step));
    }

    schedule_.publish(std::move(schedule));
}

// ---------------------------------------------------------------------------
// Preparation
// ---------------------------------------------------------------------------

void Graph::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = clampValue(sampleRate, kMinSampleRate, kMaxSampleRate);
    maxBlockSize_ = clampValue(maxBlockSize, 16, kMaxBlockSize);
    prepared_ = true;

    PrepareInfo info{ sampleRate_, maxBlockSize_, clock_ };
    for (auto& n : nodes_) n->prepare(info);

    rebuildSchedule();
}

void Graph::reset() {
    for (auto& n : nodes_) n->reset();
}

void Graph::collectGarbage() {
    schedule_.collect();

    const std::uint64_t cutoff = now();
    for (std::size_t i = retiredNodes_.size(); i-- > 0;) {
        if (cutoff > retiredNodes_[i].block + 1) {
            retiredNodes_[i] = std::move(retiredNodes_.back());
            retiredNodes_.pop_back();
        }
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void Graph::render(const TransportState& transport, int frames, std::uint64_t streamFrame,
                   const AudioBuffer* deviceInput, AudioBuffer* deviceOutput) {
    const RenderSchedule* schedule = schedule_.get();
    if (!schedule || schedule->steps.empty()) return;

    frames = clampValue(frames, 0, maxBlockSize_);
    if (frames == 0) return;

    // Buffers are allocated at maxBlockSize; narrow them to this block.
    for (const auto& b : schedule->buffers) b->setActiveFrames(frames);
    if (schedule->silenceBuffer >= 0)
        const_cast<RenderSchedule*>(schedule)->buffer(schedule->silenceBuffer).clear();

    AudioBus inBuses[kMaxPortsPerNode];
    AudioBus outBuses[kMaxPortsPerNode];

    auto* mutableSchedule = const_cast<RenderSchedule*>(schedule);

    for (const auto& step : schedule->steps) {
        Node* n = step.node;
        const int numIn = std::min(static_cast<int>(step.inputs.size()), kMaxPortsPerNode);
        const int numOut = std::min(static_cast<int>(step.outputs.size()), kMaxPortsPerNode);

        // Resolve inputs, mixing where a port has several sources or a channel
        // count that does not line up.
        for (int i = 0; i < numIn; ++i) {
            const auto& plan = step.inputs[static_cast<std::size_t>(i)];
            AudioBuffer& target = mutableSchedule->buffer(plan.bufferIndex);

            if (plan.needsMix) {
                bool first = true;
                for (int srcIndex : plan.sourceBuffers) {
                    mixInto(target, mutableSchedule->buffer(srcIndex), frames, !first);
                    first = false;
                }
            }

            inBuses[i].channels = target.data();
            inBuses[i].numChannels = target.channels();
            inBuses[i].numFrames = frames;
            inBuses[i].connected = plan.connected;
        }

        for (int i = 0; i < numOut; ++i) {
            const auto& plan = step.outputs[static_cast<std::size_t>(i)];
            AudioBuffer& target = mutableSchedule->buffer(plan.bufferIndex);
            // Nodes are not required to write every channel every block, so the
            // output starts from silence rather than last block's contents.
            target.clear();

            outBuses[i].channels = target.data();
            outBuses[i].numChannels = target.channels();
            outBuses[i].numFrames = frames;
            outBuses[i].connected = true;
        }

        if (n->bypassed()) {
            // Straight-through: input k feeds output k where both exist.
            const int pairs = std::min(numIn, numOut);
            for (int i = 0; i < pairs; ++i) {
                const auto& inPlan = step.inputs[static_cast<std::size_t>(i)];
                const auto& outPlan = step.outputs[static_cast<std::size_t>(i)];
                mixInto(mutableSchedule->buffer(outPlan.bufferIndex),
                        mutableSchedule->buffer(inPlan.bufferIndex), frames, false);
            }
            continue;
        }

        ProcessContext ctx;
        ctx.frames = frames;
        ctx.sampleRate = transport.sampleRate;
        ctx.transport = &transport;
        ctx.streamFrame = streamFrame;
        ctx.inputs = inBuses;
        ctx.numInputs = numIn;
        ctx.outputs = outBuses;
        ctx.numOutputs = numOut;
        ctx.deviceInput = deviceInput;
        ctx.deviceOutput = deviceOutput;

        n->process(ctx);
    }

    // Latch feedback sources for the next block.
    for (const auto& tap : schedule->feedbackTaps) {
        if (tap.fromBuffer < 0 || tap.toBuffer < 0) continue;
        mixInto(mutableSchedule->buffer(tap.toBuffer),
                mutableSchedule->buffer(tap.fromBuffer), frames, false);
    }
}


// ---------------------------------------------------------------------------
// Chain queries
// ---------------------------------------------------------------------------

std::vector<NodeId> downstreamChain(const Graph& graph, NodeId node, PortIndex outputPort,
                                    int maxDepth) {
    std::vector<NodeId> chain;

    NodeId current = node;
    PortIndex port = outputPort;

    for (int depth = 0; depth < maxDepth; ++depth) {
        // Exactly one consumer, or this is the end of the line. A port feeding
        // two places is a split, and what happens after it is not this stem's
        // alone - adopting it into a per-stem colour chain would be wrong.
        NodeId next = kInvalidNode;
        int consumers = 0;

        for (const Connection& c : graph.connections()) {
            if (c.sourceNode != current || c.sourcePort != port) continue;
            ++consumers;
            next = c.destNode;
        }
        if (consumers != 1 || next == kInvalidNode) break;

        // Already seen: a feedback loop. Legal in a patch, but not a chain.
        if (std::find(chain.begin(), chain.end(), next) != chain.end()) break;
        if (next == node) break;

        const Node* nextNode = graph.node(next);
        if (!nextNode) break;

        // Anything that sums several sources is a mixer, not a link in one
        // stem's chain.
        int feeders = 0;
        for (const Connection& c : graph.connections())
            if (c.destNode == next) ++feeders;
        if (feeders != 1) break;

        chain.push_back(next);

        if (nextNode->numOutputs() < 1) break;
        current = next;
        port = 0;
    }

    return chain;
}

std::vector<NodeId> allDownstreamChains(const Graph& graph, NodeId node, int maxDepth) {
    std::vector<NodeId> all;

    const Node* source = graph.node(node);
    if (!source) return all;

    for (int port = 0; port < source->numOutputs(); ++port) {
        for (NodeId id : downstreamChain(graph, node, port, maxDepth)) {
            if (std::find(all.begin(), all.end(), id) == all.end()) all.push_back(id);
        }
    }
    return all;
}

} // namespace acm

// The impact: three samples fired as one, on the frame the build lets go.
//
// A drop is almost never one sound. It is a kick for the weight, a crash or a
// reverse cymbal for the air, and something sustained underneath - a sub, a
// chord, a noise sweep - and the three have to start together to read as one
// hit. That is the whole node: three layers, one trigger, and enough control
// per layer to tune the balance between them without leaving the patch.
//
// The trigger comes from a build node by way of the transport position it
// published when it released, rather than by a call or a flag. The two nodes
// run in whatever order the graph's schedule puts them in, and a musical
// position is the one thing that means the same in both orders: a drop node
// that runs before the build simply sees the event in the next block and
// offsets into it, landing on the frame the build actually let go. There is
// also a manual trigger, so the node is useful - and testable - on its own.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/AtomicResource.h"
#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace acm {

class Graph;

class DropNode : public Node {
public:
    static constexpr int kLayers = 3;

    DropNode();

    // -- layers (message thread) -------------------------------------------
    bool loadLayer(int layer, const std::string& utf8Path, std::string* error = nullptr);
    // The same, from a buffer that is already in memory - a preview being
    // auditioned in the browser, or a snippet cut out of a stem.
    void setLayer(int layer, std::shared_ptr<SampleBuffer> sample, std::string name);
    void clearLayer(int layer);
    std::shared_ptr<SampleBuffer> layerSample(int layer) const;
    const std::string& layerPath(int layer) const;
    // The name shown on the node: the file's stem, or empty.
    const std::string& layerName(int layer) const;

    // -- wiring -------------------------------------------------------------
    // The build whose release fires this. Invalid means manual only.
    void setBuildNode(NodeId node) noexcept { buildNode_ = node; }
    NodeId buildNode() const noexcept { return buildNode_; }
    void setOwningGraph(Graph* graph) override { graph_ = graph; }

    // -- performance --------------------------------------------------------
    // Fires every loaded layer at the top of the next block. For the button on
    // the node and for anything driving this without a build.
    void trigger() noexcept { manualTrigger_.fetch_add(1, std::memory_order_release); }

    // 0..1 through the longest sounding layer, for the UI. -1 when silent.
    float progress() const noexcept { return progress_.load(std::memory_order_relaxed); }

    // -- Node ---------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

private:
    // Starts every loaded layer, `offset` frames into this block.
    void fire(int offset) noexcept;

    struct Layer {
        AtomicResource<SampleBuffer> sample;
        std::string path;
        std::string name;

        ParamIndex pGain = -1;
        ParamIndex pPitch = -1;
        ParamIndex pStart = -1;
        ParamIndex pMute = -1;

        // Audio-thread state.
        bool playing = false;
        double position = 0.0;
        double increment = 1.0;
        int startOffset = 0;   // frames still to wait before this layer speaks
    };

    std::array<Layer, kLayers> layers_;

    Graph* graph_ = nullptr;
    NodeId buildNode_ = kInvalidNode;

    ParamIndex pGain_ = -1, pDecay_ = -1;

    // Where the build's release counter had got to last time this looked. A
    // change means a release was announced; the position that came with it is
    // when it happens, which is usually several blocks away.
    std::uint32_t seenRelease_ = 0;
    bool releaseSeen_ = false;

    // An announced release that has not arrived yet. Held rather than fired
    // immediately, because the announcement comes as soon as the switch is let
    // go and the release itself lands on the next grid line - most of a bar
    // later at the settings anyone actually performs with.
    bool pending_ = false;
    double pendingPpq_ = 0.0;

    std::atomic<std::uint32_t> manualTrigger_{ 0 };
    std::uint32_t seenManual_ = 0;

    std::atomic<float> progress_{ -1.0f };

    double sampleRate_ = 48000.0;
};

} // namespace acm

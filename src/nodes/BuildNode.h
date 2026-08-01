// Build-up generator: one momentary switch that takes a section apart.
//
// Held down, it does four things at once, all of them on the musical grid:
//
//   * shortens the stem player's loop, stepping 1 -> 2 -> 4 -> 8 -> 16 over the
//     length of the build so the same bar stutters faster and faster,
//   * plays a riser sample, started at the top of the build and pitched or
//     stretched to land on the release,
//   * pushes the colour engine toward blue, which is what turns the filter up
//     and the reverb out, and
//   * optionally drops the low end out, because a build that keeps its kick
//     does not build.
//
// Released, everything returns - and by default it returns *on the next grid
// line* rather than instantly, so letting go slightly early still lands the
// drop in time. That one detail is the difference between this being usable at
// a gig and being a toy.
//
// Like the colour node this is a controller: it reaches parameters on the stem
// player and the colour node rather than sitting in their signal path. Its own
// audio output carries the riser.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/AtomicResource.h"
#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <atomic>
#include <memory>
#include <string>

namespace acm {

class Graph;

class BuildNode : public Node {
public:
    BuildNode();

    // -- riser (message thread) --------------------------------------------
    bool loadRiser(const std::string& utf8Path, std::string* error = nullptr);
    void clearRiser();
    std::shared_ptr<SampleBuffer> riser() const { return riser_.shared(); }
    const std::string& riserPath() const noexcept { return riserPath_; }

    // -- wiring (message thread) -------------------------------------------
    // Which nodes this drives. Set from the inspector; either may be invalid,
    // in which case that part of the build simply does not happen.
    void setStemPlayer(NodeId node) noexcept { stemPlayer_ = node; }
    void setColorNode(NodeId node) noexcept { colorNode_ = node; }
    NodeId stemPlayer() const noexcept { return stemPlayer_; }
    NodeId colorNode() const noexcept { return colorNode_; }

    void setOwningGraph(Graph* graph) override { graph_ = graph; }

    // -- performance -------------------------------------------------------
    // The momentary switch. Held true for as long as the build should run.
    void setEngaged(bool engaged) noexcept;
    bool engaged() const noexcept { return engaged_.load(std::memory_order_relaxed); }

    // 0..1 through the build, for the UI.
    float progress() const noexcept { return progress_.load(std::memory_order_relaxed); }

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

    enum class Release : int { NextBar = 0, NextBeat, Immediate };
    // How the loop divide climbs across the build.
    enum class Curve : int { Linear = 0, Accelerating, Stepped16 };

    static constexpr const char* kEngageParam = "engage";

    // -- which stems get chopped -------------------------------------------
    // One bit per stem slot. Stuttering the drums under a held pad is an
    // effect; stuttering everything at once is a fault, so this defaults to the
    // rhythm slots rather than to all of them.
    std::uint32_t chopMask() const noexcept { return chopMask_; }
    void setChopMask(std::uint32_t mask) noexcept { chopMask_ = mask; }
    bool chopsStem(int slot) const noexcept {
        return slot >= 0 && slot < 32 && (chopMask_ & (1u << slot)) != 0;
    }
    void toggleStem(int slot) noexcept {
        if (slot >= 0 && slot < 32) chopMask_ ^= (1u << slot);
    }

private:
    // Writes the driven parameters for the current build position. Message
    // thread, for the same reason the colour node applies there.
    void driveTargets();

    AtomicResource<SampleBuffer> riser_;
    std::string riserPath_;

    Graph* graph_ = nullptr;
    NodeId stemPlayer_ = kInvalidNode;
    NodeId colorNode_ = kInvalidNode;

    ParamIndex pEngage_ = -1, pBars_ = -1, pCurve_ = -1, pRelease_ = -1;
    ParamIndex pStartDivide_ = -1, pEndDivide_ = -1;
    ParamIndex pColorPush_ = -1, pRiserGain_ = -1, pKillLow_ = -1;

    std::atomic<bool> engaged_{ false };
    std::atomic<float> progress_{ 0.0f };

    // Published by the audio thread each block so the message thread drives the
    // targets from the same musical position the riser is being read at.
    std::atomic<float> publishedProgress_{ 0.0f };
    std::atomic<bool> publishedRunning_{ false };

    // Audio-thread state.
    bool running_ = false;
    double startPpq_ = 0.0;
    double riserPosition_ = 0.0;
    bool riserPlaying_ = false;
    // Set when the switch is released, cleared when the grid line arrives.
    bool releasePending_ = false;

    SmoothedValue riserGain_;

    // Message-thread memory of what was written, so the targets are restored
    // exactly once rather than being held down every frame.
    bool drivingTargets_ = false;
    float restoreColor_ = 0.0f;
    // Drums and bass by default: the two that a stutter reads on.
    std::uint32_t chopMask_ = 0x3u;
    // What the stem player's mask was before the build took it over.
    std::uint32_t restoreChopMask_ = 0xFFFFFFFFu;
};

} // namespace acm

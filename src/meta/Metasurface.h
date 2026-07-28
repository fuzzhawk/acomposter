// The metasurface: snapshots scattered on a 2D plane, interpolated by position.
//
// A snapshot captures the normalised value of every automatable parameter in the
// graph. Snapshots are then placed as points on a unit square. Moving the cursor
// across that square blends the snapshots by proximity and writes the result
// back into the graph, so one gesture re-poses the entire patch.
//
// Interpolating in *normalised* space is what makes this musical: a filter
// cutoff parameter skewed so that 1 kHz sits at 0.5 sweeps logarithmically
// between snapshots rather than crawling through the bottom two octaves.
// Stepped parameters (choices, switches) never blend - they snap to whichever
// snapshot currently has the most influence, because there is no meaningful
// midpoint between "forward" and "reverse".
#pragma once

#include "../core/Graph.h"
#include "../core/Json.h"
#include "../core/Types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acm {

struct Point2 {
    float x = 0.0f;
    float y = 0.0f;
};

// How proximity becomes influence.
enum class InterpolationMode : int {
    // Shepard's method. Every snapshot pulls, with strength 1/distance^power.
    // Smooth and forgiving; the default.
    InverseDistance = 0,

    // Gaussian radial basis. Influence dies off much faster, so snapshots stay
    // local and the surface has distinct regions with soft borders.
    RadialBasis,

    // Winner takes all: a hard Voronoi. Useful for scene switching rather than
    // morphing, and for stepped-heavy patches.
    Nearest,
};

const char* toString(InterpolationMode mode) noexcept;

struct Snapshot {
    SnapshotId id = kInvalidSnapshot;
    std::string name;
    Point2 position;               // in the unit square
    std::uint32_t colour = 0;      // 0xAARRGGBB, 0 = auto-assign from the palette

    // Parameter address key -> normalised value. Sparse: a snapshot only holds
    // what existed and was included when it was captured.
    std::unordered_map<std::uint64_t, float> values;

    bool contains(ParamAddress address) const {
        return values.find(address.key()) != values.end();
    }
};

class Metasurface {
public:
    Metasurface();

    // -- snapshots ---------------------------------------------------------
    SnapshotId capture(const Graph& graph, std::string name, Point2 position);
    // Re-reads the graph into an existing snapshot, keeping its position.
    bool recapture(SnapshotId id, const Graph& graph);
    bool remove(SnapshotId id);
    void clear();

    Snapshot* find(SnapshotId id);
    const Snapshot* find(SnapshotId id) const;
    const std::vector<Snapshot>& snapshots() const noexcept { return snapshots_; }
    std::size_t snapshotCount() const noexcept { return snapshots_.size(); }

    bool setPosition(SnapshotId id, Point2 position);
    bool setName(SnapshotId id, std::string name);
    bool setColour(SnapshotId id, std::uint32_t colour);

    // Drops references to parameters whose node no longer exists. Called after
    // a node is deleted so stale entries do not accumulate in saved patches.
    void pruneMissing(const Graph& graph);

    // -- parameter participation -------------------------------------------
    // Every automatable parameter takes part unless explicitly excluded. This is
    // per-address so a performer can, say, freeze one deck's pitch while the
    // rest of the patch morphs.
    void setExcluded(ParamAddress address, bool excluded);
    bool isExcluded(ParamAddress address) const;
    void clearExclusions() { excluded_.clear(); }
    std::size_t exclusionCount() const noexcept { return excluded_.size(); }

    // -- interpolation -----------------------------------------------------
    void setMode(InterpolationMode mode) noexcept { mode_ = mode; }
    InterpolationMode mode() const noexcept { return mode_; }

    // IDW falloff exponent. Higher is more local, 2 is the classic choice.
    void setPower(float power) noexcept { power_ = clampValue(power, 0.5f, 12.0f); }
    float power() const noexcept { return power_; }

    // Gaussian width for RadialBasis, as a fraction of the surface.
    void setRadius(float radius) noexcept { radius_ = clampValue(radius, 0.02f, 2.0f); }
    float radius() const noexcept { return radius_; }

    // Fills `weights` with one normalised influence per snapshot, in the order
    // snapshots() returns them. Sums to 1 whenever there is at least one.
    void computeWeights(Point2 at, std::vector<float>& weights) const;

    // Blends the snapshots at `at` and writes the result into the graph.
    void applyAt(Point2 at, Graph& graph) const;

    // The interpolated normalised value for one parameter, without touching the
    // graph. Used by the inspector to preview what the surface would do.
    bool valueAt(Point2 at, ParamAddress address, float& outNormalised) const;

    // -- cursor ------------------------------------------------------------
    void setCursor(Point2 p) noexcept { cursor_ = { clampValue(p.x, 0.0f, 1.0f), clampValue(p.y, 0.0f, 1.0f) }; }
    Point2 cursor() const noexcept { return cursor_; }

    // -- automation path ---------------------------------------------------
    // The cursor's movement can be recorded and replayed, optionally locked to
    // the transport so a recorded gesture repeats every N beats.
    struct PathPoint {
        Point2 position;
        double time = 0.0;   // seconds from the start of the recording
    };

    void beginPathRecording();
    void addPathPoint(Point2 position, double timeSeconds);
    void endPathRecording();
    bool recordingPath() const noexcept { return recordingPath_; }

    void clearPath();
    const std::vector<PathPoint>& path() const noexcept { return path_; }
    double pathDuration() const noexcept { return pathDuration_; }

    void setPathPlaying(bool playing) noexcept { pathPlaying_ = playing; }
    bool pathPlaying() const noexcept { return pathPlaying_; }

    // When locked, the path is stretched to `pathBeats` and driven by the
    // transport's musical position instead of wall-clock time.
    void setPathSynced(bool synced) noexcept { pathSynced_ = synced; }
    bool pathSynced() const noexcept { return pathSynced_; }
    void setPathBeats(double beats) noexcept { pathBeats_ = clampValue(beats, 0.25, 256.0); }
    double pathBeats() const noexcept { return pathBeats_; }

    // Advances playback and returns the cursor position to use this frame.
    // `deltaSeconds` drives free-running playback; `ppqPosition` drives synced
    // playback. Returns the unchanged cursor when the path is not playing.
    Point2 advancePath(double deltaSeconds, double ppqPosition);

    // Position along the recorded path at a normalised phase, for drawing.
    Point2 samplePath(double phase) const;

    // -- influence field ---------------------------------------------------
    // Renders the surface's regions into an RGBA8 image the UI uploads as a
    // texture: each pixel is the weighted blend of the snapshot colours, which
    // reads as soft-edged territories.
    void renderInfluenceField(int width, int height, std::vector<std::uint32_t>& outRgba) const;

    // Suggested colour for the next snapshot, cycling a fixed palette.
    std::uint32_t nextColour() const;

    // -- persistence -------------------------------------------------------
    JsonValue toJson() const;
    void fromJson(const JsonValue& in);

private:
    // Shared by computeWeights and the field renderer.
    void computeWeightsInto(Point2 at, float* weights, std::size_t count) const;

    std::vector<Snapshot> snapshots_;
    std::unordered_set<std::uint64_t> excluded_;
    SnapshotId nextId_ = 1;

    InterpolationMode mode_ = InterpolationMode::InverseDistance;
    float power_ = 2.0f;
    float radius_ = 0.35f;

    Point2 cursor_{ 0.5f, 0.5f };

    std::vector<PathPoint> path_;
    double pathDuration_ = 0.0;
    double pathPhase_ = 0.0;
    bool recordingPath_ = false;
    bool pathPlaying_ = false;
    bool pathSynced_ = true;
    double pathBeats_ = 16.0;

    // Scratch, reused so applyAt() does not allocate on every frame.
    mutable std::vector<float> weightScratch_;
};

} // namespace acm

#include "Metasurface.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

// Distinct at a glance in a dark UI, and distinguishable for the most common
// colour vision deficiencies.
constexpr std::uint32_t kPalette[] = {
    0xFF4FD1C5, // teal
    0xFFF6AD55, // amber
    0xFF9F7AEA, // violet
    0xFF63B3ED, // sky
    0xFFF56565, // coral
    0xFF68D391, // mint
    0xFFED64A6, // magenta
    0xFFECC94B, // yellow
};
constexpr std::size_t kPaletteSize = sizeof(kPalette) / sizeof(kPalette[0]);

// Below this the cursor is treated as sitting exactly on a snapshot, which
// avoids a divide-by-zero and makes the snapshot read back exactly.
constexpr float kCoincidentDistance = 1.0e-4f;

float distanceSquared(Point2 a, Point2 b) noexcept {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

} // namespace

const char* toString(InterpolationMode mode) noexcept {
    switch (mode) {
        case InterpolationMode::InverseDistance: return "inverse distance";
        case InterpolationMode::RadialBasis:     return "radial basis";
        case InterpolationMode::Nearest:         return "nearest";
    }
    return "inverse distance";
}

Metasurface::Metasurface() = default;

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

std::uint32_t Metasurface::nextColour() const {
    return kPalette[snapshots_.size() % kPaletteSize];
}

SnapshotId Metasurface::capture(const Graph& graph, std::string name, Point2 position) {
    Snapshot snapshot;
    snapshot.id = nextId_++;
    snapshot.name = name.empty() ? ("snapshot " + std::to_string(snapshot.id)) : std::move(name);
    snapshot.position = { clampValue(position.x, 0.0f, 1.0f), clampValue(position.y, 0.0f, 1.0f) };
    snapshot.colour = nextColour();

    for (const auto& node : graph.nodes()) {
        for (int p = 0; p < node->numParameters(); ++p) {
            const Parameter& parameter = node->parameter(p);
            if (!parameter.automatable()) continue;

            const ParamAddress address{ node->id(), p };
            if (isExcluded(address)) continue;

            snapshot.values[address.key()] = parameter.normalised();
        }
    }

    snapshots_.push_back(std::move(snapshot));
    return snapshots_.back().id;
}

bool Metasurface::recapture(SnapshotId id, const Graph& graph) {
    Snapshot* snapshot = find(id);
    if (!snapshot) return false;

    snapshot->values.clear();
    for (const auto& node : graph.nodes()) {
        for (int p = 0; p < node->numParameters(); ++p) {
            const Parameter& parameter = node->parameter(p);
            if (!parameter.automatable()) continue;

            const ParamAddress address{ node->id(), p };
            if (isExcluded(address)) continue;

            snapshot->values[address.key()] = parameter.normalised();
        }
    }
    return true;
}

bool Metasurface::remove(SnapshotId id) {
    const auto it = std::find_if(snapshots_.begin(), snapshots_.end(),
                                 [id](const Snapshot& s) { return s.id == id; });
    if (it == snapshots_.end()) return false;
    snapshots_.erase(it);
    return true;
}

void Metasurface::clear() {
    snapshots_.clear();
    nextId_ = 1;
}

Snapshot* Metasurface::find(SnapshotId id) {
    for (auto& s : snapshots_)
        if (s.id == id) return &s;
    return nullptr;
}

const Snapshot* Metasurface::find(SnapshotId id) const {
    for (const auto& s : snapshots_)
        if (s.id == id) return &s;
    return nullptr;
}

bool Metasurface::setPosition(SnapshotId id, Point2 position) {
    Snapshot* snapshot = find(id);
    if (!snapshot) return false;
    snapshot->position = { clampValue(position.x, 0.0f, 1.0f), clampValue(position.y, 0.0f, 1.0f) };
    return true;
}

bool Metasurface::setName(SnapshotId id, std::string name) {
    Snapshot* snapshot = find(id);
    if (!snapshot) return false;
    snapshot->name = std::move(name);
    return true;
}

bool Metasurface::setColour(SnapshotId id, std::uint32_t colour) {
    Snapshot* snapshot = find(id);
    if (!snapshot) return false;
    snapshot->colour = colour;
    return true;
}

void Metasurface::pruneMissing(const Graph& graph) {
    for (auto& snapshot : snapshots_) {
        for (auto it = snapshot.values.begin(); it != snapshot.values.end();) {
            const ParamAddress address = ParamAddress::fromKey(it->first);
            const Node* node = graph.node(address.node);
            const bool stillValid = node != nullptr
                                 && address.param >= 0
                                 && address.param < node->numParameters();
            it = stillValid ? std::next(it) : snapshot.values.erase(it);
        }
    }

    for (auto it = excluded_.begin(); it != excluded_.end();) {
        const ParamAddress address = ParamAddress::fromKey(*it);
        it = graph.node(address.node) ? std::next(it) : excluded_.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Participation
// ---------------------------------------------------------------------------

void Metasurface::setExcluded(ParamAddress address, bool excluded) {
    if (excluded) excluded_.insert(address.key());
    else excluded_.erase(address.key());
}

bool Metasurface::isExcluded(ParamAddress address) const {
    return excluded_.find(address.key()) != excluded_.end();
}

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

void Metasurface::computeWeightsInto(Point2 at, float* weights, std::size_t count) const {
    if (count == 0) return;

    if (count == 1) {
        weights[0] = 1.0f;
        return;
    }

    // Sitting on (or within a hair of) a snapshot must reproduce it exactly,
    // otherwise recalling a saved scene would come back subtly wrong.
    for (std::size_t i = 0; i < count; ++i) {
        if (distanceSquared(at, snapshots_[i].position) < kCoincidentDistance * kCoincidentDistance) {
            for (std::size_t k = 0; k < count; ++k) weights[k] = 0.0f;
            weights[i] = 1.0f;
            return;
        }
    }

    switch (mode_) {
        case InterpolationMode::Nearest: {
            std::size_t best = 0;
            float bestDistance = distanceSquared(at, snapshots_[0].position);
            for (std::size_t i = 1; i < count; ++i) {
                const float d = distanceSquared(at, snapshots_[i].position);
                if (d < bestDistance) { bestDistance = d; best = i; }
            }
            for (std::size_t i = 0; i < count; ++i) weights[i] = 0.0f;
            weights[best] = 1.0f;
            return;
        }

        case InterpolationMode::RadialBasis: {
            const float sigma = radius_;
            const float twoSigmaSquared = 2.0f * sigma * sigma;
            float total = 0.0f;

            for (std::size_t i = 0; i < count; ++i) {
                const float d2 = distanceSquared(at, snapshots_[i].position);
                const float w = std::exp(-d2 / twoSigmaSquared);
                weights[i] = w;
                total += w;
            }

            if (total <= 1.0e-20f) {
                // Everything is far outside the radius; fall back to the nearest
                // so the surface never goes undefined.
                std::size_t best = 0;
                float bestDistance = distanceSquared(at, snapshots_[0].position);
                for (std::size_t i = 1; i < count; ++i) {
                    const float d = distanceSquared(at, snapshots_[i].position);
                    if (d < bestDistance) { bestDistance = d; best = i; }
                }
                for (std::size_t i = 0; i < count; ++i) weights[i] = 0.0f;
                weights[best] = 1.0f;
                return;
            }

            const float inverse = 1.0f / total;
            for (std::size_t i = 0; i < count; ++i) weights[i] *= inverse;
            return;
        }

        case InterpolationMode::InverseDistance: {
            float total = 0.0f;
            const float halfPower = power_ * 0.5f;   // work from squared distance

            for (std::size_t i = 0; i < count; ++i) {
                const float d2 = distanceSquared(at, snapshots_[i].position);
                const float w = 1.0f / std::pow(d2, halfPower);
                weights[i] = w;
                total += w;
            }

            if (!std::isfinite(total) || total <= 0.0f) {
                const float even = 1.0f / static_cast<float>(count);
                for (std::size_t i = 0; i < count; ++i) weights[i] = even;
                return;
            }

            const float inverse = 1.0f / total;
            for (std::size_t i = 0; i < count; ++i) weights[i] *= inverse;
            return;
        }
    }
}

void Metasurface::computeWeights(Point2 at, std::vector<float>& weights) const {
    weights.assign(snapshots_.size(), 0.0f);
    computeWeightsInto(at, weights.data(), weights.size());
}

void Metasurface::applyAt(Point2 at, Graph& graph) const {
    const std::size_t count = snapshots_.size();
    if (count == 0) return;

    weightScratch_.assign(count, 0.0f);
    computeWeightsInto(at, weightScratch_.data(), count);

    // The dominant snapshot decides every stepped parameter.
    std::size_t dominant = 0;
    for (std::size_t i = 1; i < count; ++i)
        if (weightScratch_[i] > weightScratch_[dominant]) dominant = i;

    for (const auto& node : graph.nodes()) {
        for (int p = 0; p < node->numParameters(); ++p) {
            Parameter& parameter = node->parameter(p);
            if (!parameter.automatable()) continue;

            const ParamAddress address{ node->id(), p };
            if (isExcluded(address)) continue;

            const std::uint64_t key = address.key();

            if (parameter.blend() == ParamBlend::Stepped) {
                const auto it = snapshots_[dominant].values.find(key);
                if (it != snapshots_[dominant].values.end())
                    parameter.setNormalised(it->second);
                continue;
            }

            // Continuous: weighted mean over the snapshots that hold this
            // parameter, renormalised so a snapshot captured before the node
            // existed does not drag the result toward zero.
            float accumulated = 0.0f;
            float weightSum = 0.0f;

            for (std::size_t i = 0; i < count; ++i) {
                const auto it = snapshots_[i].values.find(key);
                if (it == snapshots_[i].values.end()) continue;
                accumulated += weightScratch_[i] * it->second;
                weightSum += weightScratch_[i];
            }

            if (weightSum > 1.0e-9f)
                parameter.setNormalised(accumulated / weightSum);
        }
    }
}

bool Metasurface::valueAt(Point2 at, ParamAddress address, float& outNormalised) const {
    const std::size_t count = snapshots_.size();
    if (count == 0) return false;

    weightScratch_.assign(count, 0.0f);
    computeWeightsInto(at, weightScratch_.data(), count);

    const std::uint64_t key = address.key();
    float accumulated = 0.0f;
    float weightSum = 0.0f;

    for (std::size_t i = 0; i < count; ++i) {
        const auto it = snapshots_[i].values.find(key);
        if (it == snapshots_[i].values.end()) continue;
        accumulated += weightScratch_[i] * it->second;
        weightSum += weightScratch_[i];
    }

    if (weightSum <= 1.0e-9f) return false;
    outNormalised = accumulated / weightSum;
    return true;
}

// ---------------------------------------------------------------------------
// Automation path
// ---------------------------------------------------------------------------

void Metasurface::beginPathRecording() {
    path_.clear();
    pathDuration_ = 0.0;
    pathPhase_ = 0.0;
    recordingPath_ = true;
    pathPlaying_ = false;
}

void Metasurface::addPathPoint(Point2 position, double timeSeconds) {
    if (!recordingPath_) return;

    // Drop points that are neither far enough apart nor old enough to matter;
    // a 60 Hz UI would otherwise record thousands of near-identical samples.
    if (!path_.empty()) {
        const PathPoint& last = path_.back();
        const float dx = position.x - last.position.x;
        const float dy = position.y - last.position.y;
        if (dx * dx + dy * dy < 1.0e-6f && timeSeconds - last.time < 0.1) return;
    }

    path_.push_back(PathPoint{ { clampValue(position.x, 0.0f, 1.0f), clampValue(position.y, 0.0f, 1.0f) },
                               timeSeconds });
    pathDuration_ = timeSeconds;
}

void Metasurface::endPathRecording() {
    recordingPath_ = false;
    if (path_.size() < 2) { clearPath(); return; }
    pathDuration_ = std::max(0.05, path_.back().time);
}

void Metasurface::clearPath() {
    path_.clear();
    pathDuration_ = 0.0;
    pathPhase_ = 0.0;
    pathPlaying_ = false;
    recordingPath_ = false;
}

Point2 Metasurface::samplePath(double phase) const {
    if (path_.empty()) return cursor_;
    if (path_.size() == 1) return path_.front().position;

    phase = phase - std::floor(phase);
    const double target = phase * pathDuration_;

    // Linear scan: paths are short (a gesture, not a session) and this runs once
    // per UI frame, so a binary search would be premature.
    for (std::size_t i = 1; i < path_.size(); ++i) {
        if (path_[i].time >= target) {
            const double span = path_[i].time - path_[i - 1].time;
            const double t = span > 1.0e-9 ? (target - path_[i - 1].time) / span : 0.0;
            return Point2{ lerpf(path_[i - 1].position.x, path_[i].position.x, static_cast<float>(t)),
                           lerpf(path_[i - 1].position.y, path_[i].position.y, static_cast<float>(t)) };
        }
    }
    return path_.back().position;
}

Point2 Metasurface::advancePath(double deltaSeconds, double ppqPosition) {
    if (!pathPlaying_ || path_.size() < 2) return cursor_;

    if (pathSynced_) {
        // Locked to the timeline: the gesture repeats every pathBeats_ and stays
        // in phase with the rest of the patch however long the set runs.
        pathPhase_ = pathBeats_ > 0.0 ? (ppqPosition / pathBeats_) : 0.0;
    } else if (pathDuration_ > 0.0) {
        pathPhase_ += deltaSeconds / pathDuration_;
    }

    pathPhase_ -= std::floor(pathPhase_);
    cursor_ = samplePath(pathPhase_);
    return cursor_;
}

// ---------------------------------------------------------------------------
// Influence field
// ---------------------------------------------------------------------------

void Metasurface::renderInfluenceField(int width, int height,
                                       std::vector<std::uint32_t>& outRgba) const {
    width = clampValue(width, 1, 1024);
    height = clampValue(height, 1, 1024);
    outRgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0u);

    const std::size_t count = snapshots_.size();
    if (count == 0) return;

    std::vector<float> weights(count, 0.0f);

    for (int py = 0; py < height; ++py) {
        // Sample at pixel centres so the field is symmetric about the edges.
        const float v = (static_cast<float>(py) + 0.5f) / static_cast<float>(height);

        for (int px = 0; px < width; ++px) {
            const float u = (static_cast<float>(px) + 0.5f) / static_cast<float>(width);
            computeWeightsInto(Point2{ u, v }, weights.data(), count);

            float r = 0.0f, g = 0.0f, b = 0.0f;
            float dominantWeight = 0.0f;

            for (std::size_t i = 0; i < count; ++i) {
                const std::uint32_t colour = snapshots_[i].colour;
                const float w = weights[i];
                r += w * static_cast<float>((colour >> 16) & 0xFF);
                g += w * static_cast<float>((colour >> 8) & 0xFF);
                b += w * static_cast<float>(colour & 0xFF);
                if (w > dominantWeight) dominantWeight = w;
            }

            // Alpha tracks how decided the surface is here: contested ground
            // between snapshots washes out, territory reads solid.
            const float certainty = clampValue((dominantWeight - 0.25f) / 0.75f, 0.0f, 1.0f);
            const auto alpha = static_cast<std::uint32_t>(clampValue(40.0f + certainty * 150.0f, 0.0f, 255.0f));

            outRgba[static_cast<std::size_t>(py) * static_cast<std::size_t>(width)
                    + static_cast<std::size_t>(px)] =
                (alpha << 24)
                | (static_cast<std::uint32_t>(clampValue(r, 0.0f, 255.0f)) << 16)
                | (static_cast<std::uint32_t>(clampValue(g, 0.0f, 255.0f)) << 8)
                | static_cast<std::uint32_t>(clampValue(b, 0.0f, 255.0f));
        }
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

JsonValue Metasurface::toJson() const {
    JsonValue root = JsonValue::object();
    root.set("mode", static_cast<int>(mode_));
    root.set("power", power_);
    root.set("radius", radius_);
    root.set("cursorX", cursor_.x);
    root.set("cursorY", cursor_.y);
    root.set("nextId", static_cast<double>(nextId_));

    JsonValue snapshotArray = JsonValue::array();
    for (const auto& snapshot : snapshots_) {
        JsonValue entry = JsonValue::object();
        entry.set("id", static_cast<double>(snapshot.id));
        entry.set("name", snapshot.name);
        entry.set("x", snapshot.position.x);
        entry.set("y", snapshot.position.y);
        entry.set("colour", static_cast<double>(snapshot.colour));

        // Values are written as flat [nodeId, paramIndex, value] triples: far
        // more compact than an object per parameter, and still readable.
        JsonValue values = JsonValue::array();
        // Sort so a re-saved patch produces a byte-identical file.
        std::vector<std::uint64_t> keys;
        keys.reserve(snapshot.values.size());
        for (const auto& kv : snapshot.values) keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());

        for (std::uint64_t key : keys) {
            const ParamAddress address = ParamAddress::fromKey(key);
            values.push(JsonValue(static_cast<double>(address.node)));
            values.push(JsonValue(static_cast<double>(address.param)));
            values.push(JsonValue(snapshot.values.at(key)));
        }
        entry.set("values", values);
        snapshotArray.push(entry);
    }
    root.set("snapshots", snapshotArray);

    if (!excluded_.empty()) {
        std::vector<std::uint64_t> keys(excluded_.begin(), excluded_.end());
        std::sort(keys.begin(), keys.end());

        JsonValue excludedArray = JsonValue::array();
        for (std::uint64_t key : keys) {
            const ParamAddress address = ParamAddress::fromKey(key);
            excludedArray.push(JsonValue(static_cast<double>(address.node)));
            excludedArray.push(JsonValue(static_cast<double>(address.param)));
        }
        root.set("excluded", excludedArray);
    }

    if (!path_.empty()) {
        JsonValue pathObject = JsonValue::object();
        pathObject.set("synced", pathSynced_);
        pathObject.set("beats", pathBeats_);
        pathObject.set("duration", pathDuration_);

        JsonValue points = JsonValue::array();
        for (const auto& point : path_) {
            points.push(JsonValue(point.position.x));
            points.push(JsonValue(point.position.y));
            points.push(JsonValue(point.time));
        }
        pathObject.set("points", points);
        root.set("path", pathObject);
    }

    return root;
}

void Metasurface::fromJson(const JsonValue& in) {
    clear();
    clearPath();
    excluded_.clear();

    if (!in.isObject()) return;

    mode_ = static_cast<InterpolationMode>(clampValue(in.getInt("mode", 0), 0, 2));
    setPower(in.getFloat("power", 2.0f));
    setRadius(in.getFloat("radius", 0.35f));
    cursor_ = { clampValue(in.getFloat("cursorX", 0.5f), 0.0f, 1.0f),
                clampValue(in.getFloat("cursorY", 0.5f), 0.0f, 1.0f) };

    if (const JsonValue* array = in.find("snapshots")) {
        for (const JsonValue& entry : array->items()) {
            Snapshot snapshot;
            snapshot.id = static_cast<SnapshotId>(entry.getInt64("id", 0));
            snapshot.name = entry.getString("name", "snapshot");
            snapshot.position = { clampValue(entry.getFloat("x", 0.5f), 0.0f, 1.0f),
                                  clampValue(entry.getFloat("y", 0.5f), 0.0f, 1.0f) };
            snapshot.colour = static_cast<std::uint32_t>(entry.getInt64("colour", 0));
            if (snapshot.colour == 0) snapshot.colour = kPalette[snapshots_.size() % kPaletteSize];

            if (const JsonValue* values = entry.find("values")) {
                const std::size_t n = values->size();
                for (std::size_t i = 0; i + 2 < n; i += 3) {
                    const ParamAddress address{ static_cast<NodeId>(values->at(i).asInt64(0)),
                                                static_cast<ParamIndex>(values->at(i + 1).asInt(0)) };
                    snapshot.values[address.key()] =
                        clampValue(values->at(i + 2).asFloat(0.0f), 0.0f, 1.0f);
                }
            }

            if (snapshot.id == kInvalidSnapshot) snapshot.id = nextId_;
            nextId_ = std::max(nextId_, snapshot.id + 1);
            snapshots_.push_back(std::move(snapshot));
        }
    }

    nextId_ = std::max(nextId_, static_cast<SnapshotId>(in.getInt64("nextId", 1)));

    if (const JsonValue* excluded = in.find("excluded")) {
        const std::size_t n = excluded->size();
        for (std::size_t i = 0; i + 1 < n; i += 2) {
            const ParamAddress address{ static_cast<NodeId>(excluded->at(i).asInt64(0)),
                                        static_cast<ParamIndex>(excluded->at(i + 1).asInt(0)) };
            excluded_.insert(address.key());
        }
    }

    if (const JsonValue* pathObject = in.find("path")) {
        pathSynced_ = pathObject->getBool("synced", true);
        setPathBeats(pathObject->getDouble("beats", 16.0));
        pathDuration_ = pathObject->getDouble("duration", 0.0);

        if (const JsonValue* points = pathObject->find("points")) {
            const std::size_t n = points->size();
            for (std::size_t i = 0; i + 2 < n; i += 3) {
                path_.push_back(PathPoint{ { points->at(i).asFloat(0.0f),
                                             points->at(i + 1).asFloat(0.0f) },
                                           points->at(i + 2).asDouble(0.0) });
            }
        }
        if (path_.size() < 2) clearPath();
        else if (pathDuration_ <= 0.0) pathDuration_ = std::max(0.05, path_.back().time);
    }
}

} // namespace acm

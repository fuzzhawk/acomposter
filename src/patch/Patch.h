// The .acp patch format.
//
// Patches are JSON, on purpose. A performance tool whose documents are an opaque
// blob is a tool you cannot rescue at 2am when something has gone wrong with the
// file, and the format is small enough that readability costs nothing.
//
// Node ids are preserved across a save/load round trip, because connections and
// metasurface snapshots both address parameters as (nodeId, paramIndex).
#pragma once

#include "../core/Engine.h"
#include "../core/Json.h"
#include "../control/Surface.h"
#include "../meta/Metasurface.h"

#include <string>
#include <vector>

namespace acm {

// Canvas pan and zoom, saved so a patch reopens looking the way it was left.
struct PatchViewState {
    float canvasX = 0.0f;
    float canvasY = 0.0f;
    float zoom = 1.0f;
    int activeView = 0;   // which of the main views was on screen
};

struct PatchMetadata {
    std::string title;
    std::string author;
    std::string notes;
};

struct PatchLoadResult {
    bool ok = false;
    std::string error;
    // Non-fatal problems: an unknown node type, a plugin that would not load, a
    // missing sample. The patch still opens; these are surfaced in the UI.
    std::vector<std::string> warnings;
};

namespace patch {

inline constexpr int kFormatVersion = 1;
inline constexpr const char* kFormatId = "acomposter-patch";
inline constexpr const char* kFileExtension = ".acp";

// The control surface travels with the patch: it addresses parameters by the
// same (nodeId, paramIndex) pair everything else here does, so it is only
// meaningful alongside the graph it was built against.
JsonValue save(const Engine& engine, const Metasurface& metasurface,
               const control::Surface& surface,
               const PatchViewState& view, const PatchMetadata& metadata);

// Replaces the entire contents of `engine`, `metasurface` and `surface`.
PatchLoadResult load(const JsonValue& root, Engine& engine, Metasurface& metasurface,
                     control::Surface& surface,
                     PatchViewState& view, PatchMetadata& metadata);

bool saveToFile(const std::string& utf8Path, const Engine& engine,
                const Metasurface& metasurface, const control::Surface& surface,
                const PatchViewState& view,
                const PatchMetadata& metadata, std::string* error = nullptr);

PatchLoadResult loadFromFile(const std::string& utf8Path, Engine& engine,
                             Metasurface& metasurface, control::Surface& surface,
                             PatchViewState& view, PatchMetadata& metadata);

// Builds the patch a new document starts from: a sample player and a looper
// through a crossfader into a mixer and out, which is enough to make a sound
// with three drags rather than fifteen.
void buildDefaultPatch(Engine& engine, Metasurface& metasurface);

} // namespace patch
} // namespace acm

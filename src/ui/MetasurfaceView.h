// The metasurface panel.
//
// A unit square with snapshots scattered across it. Dragging the cursor blends
// the snapshots by proximity and writes the result into the graph, so one
// gesture re-poses the whole patch. The coloured territories behind the points
// are the influence field: they show, before you move anything, which snapshot
// dominates where.
#pragma once

#include "../core/Engine.h"
#include "../gfx/Renderer.h"
#include "../meta/Metasurface.h"
#include "Ui.h"

#include <vector>

namespace acm::ui {

class MetasurfaceView {
public:
    void initialise(Engine* engine, Metasurface* metasurface, gfx::Renderer* renderer);
    void shutdown();

    // `bounds` is the whole panel; the surface itself is squared off inside it.
    void render(Ui& ui, const Rect& bounds);

    // Advances path playback and applies the surface. Called once per frame even
    // when the view is not visible, so a recorded gesture keeps running while
    // the performer works on the canvas.
    void update(float deltaSeconds);

    SnapshotId selectedSnapshot() const noexcept { return selected_; }
    void setSelectedSnapshot(SnapshotId id) noexcept { selected_ = id; }

    // Captures the graph's current state as a new snapshot at `position`.
    SnapshotId captureHere(Point2 position);

private:
    Rect surfaceRect(const Rect& bounds) const;
    Vec2 surfaceToScreen(Point2 point, const Rect& surface) const;
    Point2 screenToSurface(Vec2 screen, const Rect& surface) const;

    void refreshFieldTexture();
    void drawField(Ui& ui, const Rect& surface);
    void drawPath(Ui& ui, const Rect& surface);
    void drawSnapshots(Ui& ui, const Rect& surface);
    void drawCursor(Ui& ui, const Rect& surface);
    void drawControls(Ui& ui, const Rect& bounds);
    void drawSnapshotList(Ui& ui, const Rect& bounds);
    void handleSurfaceInput(Ui& ui, const Rect& surface);

    Engine* engine_ = nullptr;
    Metasurface* metasurface_ = nullptr;
    gfx::Renderer* renderer_ = nullptr;

    // The influence field is rendered on the CPU into a small texture and
    // stretched; it is a smooth gradient, so the resolution costs nothing
    // visually and regenerating it stays cheap.
    static constexpr int kFieldResolution = 96;
    gfx::TextureId fieldTexture_ = gfx::kNoTexture;
    std::vector<std::uint32_t> fieldPixels_;
    bool fieldDirty_ = true;

    SnapshotId selected_ = kInvalidSnapshot;
    SnapshotId draggingSnapshot_ = kInvalidSnapshot;
    bool draggingCursor_ = false;
    bool renamingSelected_ = false;
    std::string renameBuffer_;

    // Wall-clock accumulator for free-running path playback and for recording
    // timestamps.
    double pathClock_ = 0.0;
};

} // namespace acm::ui

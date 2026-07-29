#include "MetasurfaceView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace acm::ui {
namespace {

// Base sizes at 100%; everything below multiplies by the theme's display scale
// so the controls keep pace with the font inside them.
constexpr float kSnapshotRadiusBase = 9.0f;
constexpr float kCursorRadiusBase = 7.0f;
constexpr float kControlsHeightBase = 104.0f;
constexpr float kListWidthBase = 210.0f;

float snapshotRadius() { return theme().scaled(kSnapshotRadiusBase); }
float cursorRadius() { return theme().scaled(kCursorRadiusBase); }
float controlsHeight() { return theme().scaled(kControlsHeightBase); }
float listWidth() { return theme().scaled(kListWidthBase); }

} // namespace

void MetasurfaceView::initialise(Engine* engine, Metasurface* metasurface,
                                 gfx::Renderer* renderer) {
    engine_ = engine;
    metasurface_ = metasurface;
    renderer_ = renderer;

    fieldPixels_.assign(static_cast<std::size_t>(kFieldResolution) * kFieldResolution, 0u);
    if (renderer_)
        fieldTexture_ = renderer_->createTexture(fieldPixels_.data(), kFieldResolution, kFieldResolution);
    fieldDirty_ = true;
}

void MetasurfaceView::shutdown() {
    if (renderer_ && fieldTexture_ != gfx::kNoTexture) {
        renderer_->destroyTexture(fieldTexture_);
        fieldTexture_ = gfx::kNoTexture;
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

Rect MetasurfaceView::surfaceRect(const Rect& bounds) const {
    Rect area = bounds;
    area.removeFromBottom(controlsHeight());
    area.removeFromRight(listWidth());
    area = area.deflated(theme().padding);

    // Keep the surface square: the interpolation is isotropic, so a stretched
    // panel would make equal distances look unequal.
    const float side = std::min(area.width, area.height);
    return Rect{ area.left() + (area.width - side) * 0.5f,
                 area.top() + (area.height - side) * 0.5f,
                 side, side };
}

Vec2 MetasurfaceView::surfaceToScreen(Point2 point, const Rect& surface) const {
    // y is flipped so "up" on the surface is up on screen, which is what people
    // assume when they place a snapshot at the top.
    return { surface.left() + point.x * surface.width,
             surface.bottom() - point.y * surface.height };
}

Point2 MetasurfaceView::screenToSurface(Vec2 screen, const Rect& surface) const {
    return Point2{ clampValue((screen.x - surface.left()) / std::max(1.0f, surface.width), 0.0f, 1.0f),
                   clampValue((surface.bottom() - screen.y) / std::max(1.0f, surface.height), 0.0f, 1.0f) };
}

// ---------------------------------------------------------------------------
// Field
// ---------------------------------------------------------------------------

void MetasurfaceView::refreshFieldTexture() {
    if (!metasurface_ || !renderer_ || fieldTexture_ == gfx::kNoTexture) return;

    metasurface_->renderInfluenceField(kFieldResolution, kFieldResolution, fieldPixels_);

    // The metasurface renders with y increasing downward; the view draws with y
    // increasing upward, so flip the rows once here rather than on every draw.
    for (int row = 0; row < kFieldResolution / 2; ++row) {
        for (int column = 0; column < kFieldResolution; ++column) {
            std::swap(fieldPixels_[static_cast<std::size_t>(row) * kFieldResolution + column],
                      fieldPixels_[static_cast<std::size_t>(kFieldResolution - 1 - row) * kFieldResolution
                                   + column]);
        }
    }

    // The field is RGBA in memory but the shader samples R8G8B8A8, so swap the
    // red and blue channels the metasurface packed as 0xAARRGGBB.
    for (std::uint32_t& pixel : fieldPixels_) {
        const std::uint32_t a = (pixel >> 24) & 0xFF;
        const std::uint32_t r = (pixel >> 16) & 0xFF;
        const std::uint32_t g = (pixel >> 8) & 0xFF;
        const std::uint32_t b = pixel & 0xFF;
        pixel = (a << 24) | (b << 16) | (g << 8) | r;
    }

    renderer_->updateTexture(fieldTexture_, fieldPixels_.data(), kFieldResolution, kFieldResolution);
    fieldDirty_ = false;
}

void MetasurfaceView::drawField(Ui& ui, const Rect& surface) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(surface, t.panelSunken, t.cornerRadiusLarge);

    if (fieldDirty_) refreshFieldTexture();

    if (fieldTexture_ != gfx::kNoTexture && metasurface_->snapshotCount() > 0)
        list.addImage(fieldTexture_, surface);

    // Grid overlay: quarters, so a snapshot's position can be judged by eye.
    for (int i = 1; i < 4; ++i) {
        const float fraction = static_cast<float>(i) / 4.0f;
        list.addRectFilled(Rect{ surface.left() + surface.width * fraction, surface.top(),
                                 1.0f, surface.height }, t.canvasGrid.withAlpha(0.5f));
        list.addRectFilled(Rect{ surface.left(), surface.top() + surface.height * fraction,
                                 surface.width, 1.0f }, t.canvasGrid.withAlpha(0.5f));
    }

    list.addRect(surface, t.border, t.borderWidth, t.cornerRadiusLarge);

    if (metasurface_->snapshotCount() == 0) {
        list.addTextClipped(ui.font(t.fontUi), surface, t.textFaint,
                            "no snapshots yet - set up the patch, then capture one",
                            DrawList::Align::Centre);
    }
}

void MetasurfaceView::drawPath(Ui& ui, const Rect& surface) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    const auto& path = metasurface_->path();
    if (path.size() < 2) return;

    std::vector<Vec2> points;
    points.reserve(path.size());
    for (const auto& point : path) points.push_back(surfaceToScreen(point.position, surface));

    const Colour colour = metasurface_->pathPlaying() ? t.control : t.control.withAlpha(0.4f);
    list.addPolyline(points.data(), static_cast<int>(points.size()), colour, false, 1.6f);

    // A marker at the start so the loop's phase is legible when it is running.
    list.addCircle(points.front(), 3.5f, colour, 1.4f);
}

void MetasurfaceView::drawSnapshots(Ui& ui, const Rect& surface) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    for (const Snapshot& snapshot : metasurface_->snapshots()) {
        const Vec2 position = surfaceToScreen(snapshot.position, surface);
        const Colour colour = Colour::fromArgb(snapshot.colour);
        const bool isSelected = snapshot.id == selected_;

        const Rect hitRect{ position.x - snapshotRadius(), position.y - snapshotRadius(),
                            snapshotRadius() * 2.0f, snapshotRadius() * 2.0f };
        const bool hovered = ui.hovering(hitRect);

        if (isSelected || hovered)
            list.addGlow(hitRect, colour.withAlpha(0.6f), 10.0f, snapshotRadius(), 4);

        list.addCircleFilled(position, snapshotRadius(), t.panelSunken);
        list.addCircleFilled(position, snapshotRadius() - 2.5f, colour);
        list.addCircle(position, snapshotRadius(), isSelected ? t.text : colour.brightened(1.2f),
                       isSelected ? 2.0f : 1.3f);

        // Labels sit above the point so a cluster stays readable.
        const Rect labelRect{ position.x - 60.0f, position.y - snapshotRadius() - 15.0f,
                              120.0f, 13.0f };
        list.addTextClipped(ui.font(t.fontSmall), labelRect,
                            isSelected ? t.text : t.textDim, snapshot.name,
                            DrawList::Align::Centre);

        if (hovered) {
            char detail[128];
            std::snprintf(detail, sizeof(detail), "%s - %zu parameters",
                          snapshot.name.c_str(), snapshot.values.size());
            ui.setTooltip(detail);
        }
    }
}

void MetasurfaceView::drawCursor(Ui& ui, const Rect& surface) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    const Vec2 position = surfaceToScreen(metasurface_->cursor(), surface);

    // Crosshair rails out to the edges: the exact position matters, and a bare
    // dot is hard to place precisely.
    list.addRectFilled(Rect{ surface.left(), position.y - 0.5f, surface.width, 1.0f },
                       t.text.withAlpha(0.18f));
    list.addRectFilled(Rect{ position.x - 0.5f, surface.top(), 1.0f, surface.height },
                       t.text.withAlpha(0.18f));

    list.addCircle(position, cursorRadius() + 4.0f, t.text.withAlpha(0.35f), 1.0f);
    list.addCircleFilled(position, cursorRadius(), t.background);
    list.addCircle(position, cursorRadius(), t.text, 2.0f);
    list.addCircleFilled(position, 2.0f, t.text);

    if (metasurface_->recordingPath())
        list.addCircle(position, cursorRadius() + 8.0f, t.recording, 1.8f);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void MetasurfaceView::handleSurfaceInput(Ui& ui, const Rect& surface) {
    const InputState& input = ui.input();
    const bool over = ui.hovering(surface);

    // -- grab a snapshot ---------------------------------------------------
    if (over && input.mousePressed[static_cast<int>(MouseButton::Left)] && !ui.pointerCaptured()) {
        SnapshotId hit = kInvalidSnapshot;
        for (const Snapshot& snapshot : metasurface_->snapshots()) {
            const Vec2 position = surfaceToScreen(snapshot.position, surface);
            if ((input.mousePosition - position).length() <= snapshotRadius() + 3.0f) {
                hit = snapshot.id;
                break;
            }
        }

        if (hit != kInvalidSnapshot) {
            selected_ = hit;
            // Alt-click recalls a snapshot exactly rather than moving it, which
            // is the fastest way to audition one.
            if (input.alt) {
                if (const Snapshot* snapshot = metasurface_->find(hit)) {
                    metasurface_->setCursor(snapshot->position);
                    metasurface_->applyAt(snapshot->position, engine_->graph());
                }
            } else {
                draggingSnapshot_ = hit;
            }
        } else {
            draggingCursor_ = true;
        }
    }

    // -- drag --------------------------------------------------------------
    if (draggingSnapshot_ != kInvalidSnapshot) {
        metasurface_->setPosition(draggingSnapshot_, screenToSurface(input.mousePosition, surface));
        fieldDirty_ = true;
        ui.setCursor(Cursor::Hand);

        if (!input.mouseDown[static_cast<int>(MouseButton::Left)])
            draggingSnapshot_ = kInvalidSnapshot;
    }

    if (draggingCursor_) {
        const Point2 position = screenToSurface(input.mousePosition, surface);
        metasurface_->setCursor(position);
        metasurface_->applyAt(position, engine_->graph());

        if (metasurface_->recordingPath())
            metasurface_->addPathPoint(position, pathClock_);

        ui.setCursor(Cursor::Crosshair);

        if (!input.mouseDown[static_cast<int>(MouseButton::Left)])
            draggingCursor_ = false;
    }

    // -- capture where clicked --------------------------------------------
    if (over && input.mouseDoubleClicked[static_cast<int>(MouseButton::Left)] && !input.alt) {
        captureHere(screenToSurface(input.mousePosition, surface));
    }

    if (over && input.mousePressed[static_cast<int>(MouseButton::Right)]) {
        // Right-click removes the snapshot under the pointer.
        for (const Snapshot& snapshot : metasurface_->snapshots()) {
            const Vec2 position = surfaceToScreen(snapshot.position, surface);
            if ((input.mousePosition - position).length() <= snapshotRadius() + 3.0f) {
                metasurface_->remove(snapshot.id);
                if (selected_ == snapshot.id) selected_ = kInvalidSnapshot;
                fieldDirty_ = true;
                break;
            }
        }
    }

    if (over && !draggingCursor_ && draggingSnapshot_ == kInvalidSnapshot)
        ui.setCursor(Cursor::Crosshair);
}

SnapshotId MetasurfaceView::captureHere(Point2 position) {
    if (!metasurface_ || !engine_) return kInvalidSnapshot;

    const std::string name = "snap " + std::to_string(metasurface_->snapshotCount() + 1);
    const SnapshotId id = metasurface_->capture(engine_->graph(), name, position);
    selected_ = id;
    fieldDirty_ = true;
    return id;
}

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------

void MetasurfaceView::drawControls(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();

    Rect area = bounds;
    area.removeFromRight(listWidth());
    Rect strip = area.removeFromBottom(controlsHeight()).deflated(t.padding);

    // -- row one: capture and interpolation --------------------------------
    Rect row = strip.removeFromTop(t.scaled(24.0f));

    if (ui.button(ui.id("meta.capture"), row.removeFromLeft(t.scaled(96.0f)), "capture",
                  Ui::ButtonStyle::Primary))
        captureHere(metasurface_->cursor());
    if (ui.isHot(ui.id("meta.capture")))
        ui.setTooltip("Store every automatable parameter in the patch at the cursor's position");
    row.removeFromLeft(6.0f);

    const bool hasSelection = metasurface_->find(selected_) != nullptr;

    if (ui.button(ui.id("meta.update"), row.removeFromLeft(t.scaled(74.0f)), "update",
                  Ui::ButtonStyle::Normal, false, hasSelection)) {
        metasurface_->recapture(selected_, engine_->graph());
        fieldDirty_ = true;
        ui.notify("snapshot updated", t.accent, 2.0f);
    }
    row.removeFromLeft(6.0f);

    if (ui.button(ui.id("meta.remove"), row.removeFromLeft(t.scaled(70.0f)), "remove",
                  Ui::ButtonStyle::Danger, false, hasSelection)) {
        metasurface_->remove(selected_);
        selected_ = kInvalidSnapshot;
        fieldDirty_ = true;
    }
    row.removeFromLeft(16.0f);

    // Interpolation mode.
    const Rect modeArea = row.removeFromLeft(t.scaled(158.0f));
    static const std::vector<std::string> modes = { "inverse distance", "radial basis", "nearest" };
    int mode = static_cast<int>(metasurface_->mode());
    if (ui.combo(ui.id("meta.mode"), modeArea, modes, mode)) {
        metasurface_->setMode(static_cast<InterpolationMode>(mode));
        fieldDirty_ = true;
    }
    row.removeFromLeft(8.0f);

    // The shaping control depends on the mode, because "power" means nothing to
    // radial basis and "radius" means nothing to inverse distance.
    if (metasurface_->mode() == InterpolationMode::RadialBasis) {
        const Rect radiusArea = row.removeFromLeft(std::min(190.0f, row.width));
        float normalised = (metasurface_->radius() - 0.02f) / 0.98f;
        if (ui.sliderNormalised(ui.id("meta.radius"), radiusArea, normalised, t.control)) {
            metasurface_->setRadius(0.02f + normalised * 0.98f);
            fieldDirty_ = true;
        }
        char text[48];
        std::snprintf(text, sizeof(text), "radius %.2f", static_cast<double>(metasurface_->radius()));
        ui.labelDim(radiusArea, text, DrawList::Align::Centre);
    } else if (metasurface_->mode() == InterpolationMode::InverseDistance) {
        const Rect powerArea = row.removeFromLeft(std::min(190.0f, row.width));
        float normalised = (metasurface_->power() - 0.5f) / 11.5f;
        if (ui.sliderNormalised(ui.id("meta.power"), powerArea, normalised, t.control)) {
            metasurface_->setPower(0.5f + normalised * 11.5f);
            fieldDirty_ = true;
        }
        char text[48];
        std::snprintf(text, sizeof(text), "focus %.1f", static_cast<double>(metasurface_->power()));
        ui.labelDim(powerArea, text, DrawList::Align::Centre);
    }

    strip.removeFromTop(6.0f);

    // -- row two: path automation ------------------------------------------
    Rect pathRow = strip.removeFromTop(t.scaled(24.0f));

    const bool recording = metasurface_->recordingPath();
    if (ui.iconButton(ui.id("meta.pathrec"), pathRow.removeFromLeft(t.scaled(28.0f)), Ui::Icon::Record,
                      recording ? t.recording : t.textDim, recording)) {
        if (recording) metasurface_->endPathRecording();
        else { metasurface_->beginPathRecording(); pathClock_ = 0.0; }
    }
    if (ui.isHot(ui.id("meta.pathrec")))
        ui.setTooltip("Record the cursor's movement as a repeatable gesture");
    pathRow.removeFromLeft(4.0f);

    const bool playing = metasurface_->pathPlaying();
    if (ui.iconButton(ui.id("meta.pathplay"), pathRow.removeFromLeft(t.scaled(28.0f)), Ui::Icon::Play,
                      playing ? t.accent : t.textDim, playing,
                      metasurface_->path().size() >= 2))
        metasurface_->setPathPlaying(!playing);
    pathRow.removeFromLeft(4.0f);

    if (ui.iconButton(ui.id("meta.pathclear"), pathRow.removeFromLeft(t.scaled(28.0f)), Ui::Icon::Trash,
                      t.textDim, false, !metasurface_->path().empty()))
        metasurface_->clearPath();
    pathRow.removeFromLeft(12.0f);

    bool synced = metasurface_->pathSynced();
    if (ui.checkbox(ui.id("meta.pathsync"), pathRow.removeFromLeft(t.scaled(90.0f)), "sync", synced))
        metasurface_->setPathSynced(synced);
    pathRow.removeFromLeft(6.0f);

    if (metasurface_->pathSynced()) {
        const Rect beatsArea = pathRow.removeFromLeft(std::min(170.0f, pathRow.width));
        float normalised = static_cast<float>(std::log2(metasurface_->pathBeats() / 0.25) / 10.0);
        if (ui.sliderNormalised(ui.id("meta.pathbeats"), beatsArea, normalised, t.control))
            metasurface_->setPathBeats(0.25 * std::pow(2.0, normalised * 10.0));

        char text[48];
        std::snprintf(text, sizeof(text), "%.2f beats per lap", metasurface_->pathBeats());
        ui.labelDim(beatsArea, text, DrawList::Align::Centre);
    }
}

void MetasurfaceView::drawSnapshotList(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();

    Rect panel = bounds;
    panel = panel.removeFromRight(listWidth()).deflated(t.padding);
    ui.panel(panel);
    panel = panel.deflated(t.smallPadding);

    ui.label(panel.removeFromTop(t.scaled(18.0f)), "snapshots", t.textDim, t.fontUiBold);
    panel.removeFromTop(4.0f);

    // Renaming the selected snapshot happens here rather than on the surface,
    // where a text field would be in the way of the gesture.
    if (selected_ != kInvalidSnapshot) {
        const Rect nameRow = panel.removeFromTop(t.scaled(22.0f));
        if (const Snapshot* snapshot = metasurface_->find(selected_)) {
            if (!renamingSelected_) renameBuffer_ = snapshot->name;

            const UiId field = ui.id("meta.rename");
            renamingSelected_ = ui.editingText(field);
            if (ui.textField(field, nameRow, renameBuffer_, "snapshot name")) {
                metasurface_->setName(selected_, renameBuffer_);
                renamingSelected_ = false;
            }
        }
        panel.removeFromTop(6.0f);
    }

    const auto& snapshots = metasurface_->snapshots();
    const float contentHeight = static_cast<float>(snapshots.size()) * t.scaled(24.0f);

    Rect content = ui.beginScroll(ui.id("meta.list"), panel, contentHeight);

    for (const Snapshot& snapshot : snapshots) {
        const Rect row = content.removeFromTop(t.scaled(23.0f));
        content.removeFromTop(1.0f);

        const bool isSelected = snapshot.id == selected_;
        const UiId rowId = ui.idFrom(&snapshot, 1);

        bool hovered = false, held = false;
        if (ui.buttonBehaviour(rowId, row, hovered, held)) {
            selected_ = snapshot.id;
            // Selecting from the list also recalls it, which is what makes the
            // list usable as a scene launcher.
            metasurface_->setCursor(snapshot.position);
            metasurface_->applyAt(snapshot.position, engine_->graph());
        }

        if (isSelected) ui.draw().addRectFilled(row, t.accent.withAlpha(0.12f), t.cornerRadius);
        else if (hovered) ui.draw().addRectFilled(row, t.widgetHover, t.cornerRadius);

        Rect rowContent = row.deflated(4.0f);
        const Rect swatch = rowContent.removeFromLeft(10.0f);
        ui.draw().addCircleFilled(swatch.centre(), 4.0f, Colour::fromArgb(snapshot.colour));
        rowContent.removeFromLeft(6.0f);

        ui.draw().addTextClipped(ui.font(t.fontUi), rowContent,
                                 isSelected ? t.text : t.textDim, snapshot.name);
    }

    ui.endScroll();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void MetasurfaceView::update(float deltaSeconds) {
    if (!metasurface_ || !engine_) return;

    pathClock_ += static_cast<double>(deltaSeconds);

    if (metasurface_->pathPlaying() && metasurface_->path().size() >= 2) {
        const TransportState transport = engine_->transport().snapshot();
        const Point2 position = metasurface_->advancePath(static_cast<double>(deltaSeconds),
                                                          transport.ppqPosition);
        metasurface_->applyAt(position, engine_->graph());
    }
}

void MetasurfaceView::render(Ui& ui, const Rect& bounds) {
    if (!metasurface_ || !engine_) return;

    const Theme& t = theme();
    ui.draw().addRectFilled(bounds, t.background);

    const Rect surface = surfaceRect(bounds);

    drawField(ui, surface);
    drawPath(ui, surface);
    handleSurfaceInput(ui, surface);
    drawSnapshots(ui, surface);
    drawCursor(ui, surface);

    drawControls(ui, bounds);
    drawSnapshotList(ui, bounds);
}

} // namespace acm::ui

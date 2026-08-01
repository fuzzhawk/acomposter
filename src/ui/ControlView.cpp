#include "ControlView.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace acm::ui {
namespace {

float toolbarHeight() { return theme().scaled(34.0f); }
float tabsHeight() { return theme().scaled(26.0f); }
float inspectorWidth() { return theme().scaled(250.0f); }

// The kinds offered in the palette, in the order they are worth reaching for.
constexpr control::ControlKind kPalette[] = {
    control::ControlKind::Knob,
    control::ControlKind::Fader,
    control::ControlKind::Button,
    control::ControlKind::XYPad,
    control::ControlKind::Metasurface,
    control::ControlKind::Label,
};

// A new control of each kind wants a different amount of room.
void defaultSize(control::ControlKind kind, int& width, int& height) {
    switch (kind) {
        case control::ControlKind::Fader:       width = 1; height = 4; break;
        case control::ControlKind::Button:      width = 2; height = 1; break;
        case control::ControlKind::XYPad:       width = 4; height = 4; break;
        case control::ControlKind::Metasurface: width = 5; height = 5; break;
        case control::ControlKind::Label:       width = 3; height = 1; break;
        default:                                width = 2; height = 2; break;
    }
}

} // namespace

void ControlView::initialise(Engine* engine, control::Surface* surface,
                             MetasurfaceView* metasurfaceView) {
    engine_ = engine;
    surface_ = surface;
    metasurfaceView_ = metasurfaceView;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

Rect ControlView::gridCell(const Rect& area, int column, int row,
                           int width, int height) const {
    if (!surface_) return {};

    const float cellWidth = area.width / static_cast<float>(std::max(1, surface_->columns()));
    const float cellHeight = area.height / static_cast<float>(std::max(1, surface_->rows()));

    return Rect{ area.left() + cellWidth * static_cast<float>(column),
                 area.top() + cellHeight * static_cast<float>(row),
                 cellWidth * static_cast<float>(width),
                 cellHeight * static_cast<float>(height) };
}

// ---------------------------------------------------------------------------
// Learn
// ---------------------------------------------------------------------------

bool ControlView::completeLearn(ParamAddress address) {
    if (learnControl_ == 0 || !surface_ || !engine_) return false;
    if (!address.valid()) return false;

    const bool bound = surface_->bind(learnControl_, address, engine_->graph(), learnSecondAxis_);
    learnControl_ = 0;

    if (bound && onModified) onModified();
    return bound;
}

std::string ControlView::describe(ParamAddress address) const {
    if (!engine_) return {};

    const Node* node = engine_->graph().node(address.node);
    if (!node) return "(missing)";
    if (address.param < 0 || address.param >= node->numParameters()) return "(missing)";

    return node->name() + " . " + node->parameter(address.param).name();
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void ControlView::drawToolbar(Ui& ui, Rect& area) {
    const Theme& t = theme();
    Rect bar = area.removeFromTop(toolbarHeight());

    ui.draw().addRectFilled(bar, t.panelHeader);
    ui.draw().addRectFilled(Rect{ bar.left(), bar.bottom() - 1.0f, bar.width, 1.0f }, t.border);

    Rect row = bar.deflated(t.smallPadding);

    if (ui.button(ui.id("control.edit"), row.removeFromLeft(t.scaled(64.0f)),
                  editing_ ? "playing" : "edit",
                  editing_ ? Ui::ButtonStyle::Primary : Ui::ButtonStyle::Normal, editing_))
        setEditing(!editing_);
    if (ui.isHot(ui.id("control.edit")))
        ui.setTooltip(editing_ ? "Leave edit mode" : "Add, move and bind controls");

    row.removeFromLeft(t.scaled(10.0f));

    if (!editing_) {
        // Out of edit mode the toolbar says what the surface is rather than
        // offering ways to change it.
        const control::Page* page = surface_ ? surface_->page(surface_->activePage()) : nullptr;
        char summary[128];
        std::snprintf(summary, sizeof(summary), "%d control%s",
                      page ? static_cast<int>(page->controls.size()) : 0,
                      (page && page->controls.size() == 1) ? "" : "s");
        ui.labelDim(row, summary);
        return;
    }

    // -- palette -----------------------------------------------------------
    ui.label(row.removeFromLeft(t.scaled(30.0f)), "add", t.textFaint, t.fontSmall);

    for (const control::ControlKind kind : kPalette) {
        const char* name = control::toString(kind);
        const float width = ui.font(t.fontSmall).textWidth(name) + t.scaled(16.0f);
        const Rect button = row.removeFromLeft(std::min(width, row.width));
        row.removeFromLeft(t.scaled(3.0f));

        if (ui.button(ui.id(std::string("control.add.") + name), button, name)) {
            int w = 2, h = 2;
            defaultSize(kind, w, h);

            // First free cell, scanned left to right and top to bottom. Stacking
            // the next one below the last was simpler and wrong: once the column
            // ran out of rows it wrapped to 0,0 and dropped the new control on
            // top of the first, which looked exactly like the add having failed.
            int column = 0, rowIndex = 0;
            const control::Page* page = surface_->page(surface_->activePage());

            const auto free = [&](int c, int r) {
                if (!page) return true;
                for (const control::Control& existing : page->controls) {
                    if (c < existing.column + existing.width && c + w > existing.column
                        && r < existing.row + existing.height && r + h > existing.row)
                        return false;
                }
                return true;
            };

            bool placed = false;
            for (int r = 0; r + h <= surface_->rows() && !placed; ++r) {
                for (int c = 0; c + w <= surface_->columns() && !placed; ++c) {
                    if (!free(c, r)) continue;
                    column = c;
                    rowIndex = r;
                    placed = true;
                }
            }

            // A full page still gets the control, on top of whatever is at the
            // origin. Refusing to add it would leave the button doing nothing
            // with no way to find out why.
            selected_ = surface_->add(kind, name, column, rowIndex, w, h);
            if (!placed)
                ui.notify("no free space - move it somewhere", t.warning, 3.0f);
            if (onModified) onModified();
        }
    }

    // -- grid size ---------------------------------------------------------
    Rect right = row;
    int rows = surface_->rows();
    int columns = surface_->columns();

    if (ui.intField(ui.id("control.rows"), right.removeFromRight(t.scaled(40.0f)), rows, 3, 32)) {
        surface_->setGrid(columns, rows);
        if (onModified) onModified();
    }
    ui.label(right.removeFromRight(t.scaled(16.0f)), "x", t.textFaint, t.fontSmall);
    if (ui.intField(ui.id("control.columns"), right.removeFromRight(t.scaled(40.0f)),
                    columns, 4, 48)) {
        surface_->setGrid(columns, rows);
        if (onModified) onModified();
    }
    ui.label(right.removeFromRight(t.scaled(34.0f)), "grid", t.textFaint, t.fontSmall);
}

void ControlView::drawPageTabs(Ui& ui, Rect& area) {
    const Theme& t = theme();
    if (!surface_) return;

    Rect bar = area.removeFromTop(tabsHeight());
    ui.draw().addRectFilled(bar, t.panel);

    Rect row = bar.deflated(t.scaled(3.0f));

    for (int i = 0; i < surface_->pageCount() && row.width > t.scaled(40.0f); ++i) {
        const control::Page* page = surface_->page(i);
        if (!page) continue;

        const float width = std::min(ui.font(t.fontSmall).textWidth(page->name) + t.scaled(22.0f),
                                     row.width);
        const Rect tab = row.removeFromLeft(width);
        row.removeFromLeft(t.scaled(2.0f));

        if (ui.button(ui.id("control.page." + std::to_string(i)), tab, page->name,
                      Ui::ButtonStyle::Toggle, i == surface_->activePage())) {
            surface_->setActivePage(i);
            selected_ = 0;
            // The new page's controls show where their parameters actually are
            // rather than where they were left, so arriving on a page never
            // moves anything.
            if (engine_) surface_->adoptAllFromGraph(engine_->graph());
        }
    }

    if (!editing_) return;

    if (ui.iconButton(ui.id("control.page.add"), row.removeFromLeft(t.scaled(22.0f)),
                      Ui::Icon::Plus, t.textDim)) {
        surface_->setActivePage(surface_->addPage("page " + std::to_string(surface_->pageCount() + 1)));
        selected_ = 0;
        if (onModified) onModified();
    }

    if (surface_->pageCount() > 1
        && ui.iconButton(ui.id("control.page.remove"), row.removeFromLeft(t.scaled(22.0f)),
                         Ui::Icon::Trash, t.textFaint)) {
        surface_->removePage(surface_->activePage());
        selected_ = 0;
        if (onModified) onModified();
    }
}

void ControlView::drawGrid(Ui& ui, const Rect& area) const {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    for (int c = 1; c < surface_->columns(); ++c) {
        const float x = area.left() + area.width * static_cast<float>(c)
                      / static_cast<float>(surface_->columns());
        list.addRectFilled(Rect{ x, area.top(), 1.0f, area.height }, t.canvasGrid);
    }
    for (int r = 1; r < surface_->rows(); ++r) {
        const float y = area.top() + area.height * static_cast<float>(r)
                      / static_cast<float>(surface_->rows());
        list.addRectFilled(Rect{ area.left(), y, area.width, 1.0f }, t.canvasGrid);
    }
}

void ControlView::drawControl(Ui& ui, const Rect& cell, control::Control& control) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    const Colour accent = control.colour != 0 ? Colour::fromArgb(control.colour) : t.accent;
    const Rect inner = cell.deflated(t.scaled(4.0f));
    const UiId id = ui.id("control.item." + std::to_string(control.id));

    // Unbound controls are drawn faint. A surface half-built should look half
    // built rather than looking finished and doing nothing.
    const bool bound = !control.targets.empty() || !control.targetsY.empty();
    const Colour colour = bound ? accent : accent.withAlpha(0.35f);

    switch (control.kind) {
        case control::ControlKind::Label: {
            list.addTextClipped(ui.font(t.fontUiBold), inner, t.textDim, control.name,
                                DrawList::Align::Centre);
            break;
        }

        case control::ControlKind::Metasurface: {
            // Squared off inside its cell for the same reason the full panel
            // does it: the interpolation is isotropic and a stretched pad makes
            // equal distances look unequal.
            const float side = std::min(inner.width, inner.height);
            const Rect pad{ inner.left() + (inner.width - side) * 0.5f,
                            inner.top() + (inner.height - side) * 0.5f, side, side };
            if (metasurfaceView_) metasurfaceView_->renderPad(ui, pad);
            break;
        }

        case control::ControlKind::Button: {
            bool hovered = false, held = false;
            const bool clicked = ui.buttonBehaviour(id, inner, hovered, held);

            if (control.momentary) {
                // Held, not toggled: the value follows the button for as long
                // as it is down, which is what a build's engage switch needs.
                const float wanted = ui.isActive(id) ? 1.0f : 0.0f;
                if (wanted != control.value && engine_)
                    surface_->setValue(control.id, wanted, engine_->graph());
            } else if (clicked && engine_) {
                surface_->setValue(control.id, control.value > 0.5f ? 0.0f : 1.0f,
                                   engine_->graph());
            }

            const bool on = control.value > 0.5f;
            Colour fill = on ? colour.withAlpha(0.42f) : t.widgetBackground;
            if (hovered) fill = fill.brightened(1.35f);
            list.addRectFilled(inner, fill, t.cornerRadius);
            list.addRect(inner, on ? colour : t.border, 1.0f, t.cornerRadius);
            list.addTextClipped(ui.font(t.fontUi), inner, on ? t.text : t.textDim,
                                control.name, DrawList::Align::Centre);
            break;
        }

        case control::ControlKind::Fader: {
            Rect track = inner;
            const Rect labelRect = track.removeFromBottom(t.scaled(14.0f));

            float value = control.value;
            if (ui.sliderNormalised(id, track, value, colour, true) && engine_)
                surface_->setValue(control.id, value, engine_->graph());

            list.addTextClipped(ui.font(t.fontSmall), labelRect, t.textDim, control.name,
                                DrawList::Align::Centre);
            break;
        }

        case control::ControlKind::XYPad: {
            list.addRectFilled(inner, t.panelSunken, t.cornerRadius);

            bool hovered = false, held = false;
            ui.buttonBehaviour(id, inner, hovered, held);
            if (ui.isActive(id) && engine_) {
                const Vec2 pointer = ui.input().mousePosition;
                surface_->setValueXY(
                    control.id,
                    clampValue((pointer.x - inner.left()) / std::max(1.0f, inner.width), 0.0f, 1.0f),
                    clampValue((inner.bottom() - pointer.y) / std::max(1.0f, inner.height),
                               0.0f, 1.0f),
                    engine_->graph());
            }

            for (int i = 1; i < 4; ++i) {
                const float fraction = static_cast<float>(i) / 4.0f;
                list.addRectFilled(Rect{ inner.left() + inner.width * fraction, inner.top(),
                                         1.0f, inner.height }, t.canvasGrid);
                list.addRectFilled(Rect{ inner.left(), inner.top() + inner.height * fraction,
                                         inner.width, 1.0f }, t.canvasGrid);
            }

            const Vec2 handle{ inner.left() + inner.width * control.value,
                               inner.bottom() - inner.height * control.valueY };
            list.addCircleFilled(handle, t.scaled(7.0f), colour);
            list.addCircle(handle, t.scaled(7.0f), t.text, 1.5f);

            list.addRect(inner, t.border, 1.0f, t.cornerRadius);
            list.addTextClipped(ui.font(t.fontSmall),
                                Rect{ inner.left(), inner.top() + t.scaled(2.0f),
                                      inner.width, t.scaled(13.0f) },
                                t.textFaint, control.name, DrawList::Align::Centre);
            break;
        }

        case control::ControlKind::Knob:
        default: {
            Rect knobArea = inner;
            const Rect labelRect = knobArea.removeFromBottom(t.scaled(14.0f));

            float value = control.value;
            if (ui.knob(id, knobArea, value, colour, 0.0f) && engine_)
                surface_->setValue(control.id, value, engine_->graph());

            list.addTextClipped(ui.font(t.fontSmall), labelRect, t.textDim, control.name,
                                DrawList::Align::Centre);
            break;
        }
    }
}

void ControlView::drawEditOverlay(Ui& ui, const Rect& cell, control::Control& control) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    const bool isSelected = control.id == selected_;
    list.addRect(cell, isSelected ? t.selection : t.border.brightened(1.4f),
                 isSelected ? 2.0f : 1.0f, t.cornerRadius);

    // A grab strip along the top rather than the whole cell: dragging has to be
    // possible without the control underneath moving, and a knob that jumps to
    // 0.8 every time it is repositioned is not a layout tool.
    const Rect grip{ cell.left(), cell.top(), cell.width, t.scaled(12.0f) };
    const UiId gripId = ui.id("control.grip." + std::to_string(control.id));

    bool hovered = false, held = false;
    ui.buttonBehaviour(gripId, grip, hovered, held);

    if (ui.isActive(gripId)) {
        if (dragging_ != control.id) {
            dragging_ = control.id;
            selected_ = control.id;
            resizing_ = false;

            // Remember where in the control the grab happened.
            const float cellWidth = cell.width / static_cast<float>(std::max(1, control.width));
            const float cellHeight = cell.height / static_cast<float>(std::max(1, control.height));
            dragOffsetColumn_ = static_cast<int>((ui.input().mousePosition.x - cell.left())
                                                 / std::max(1.0f, cellWidth));
            dragOffsetRow_ = static_cast<int>((ui.input().mousePosition.y - cell.top())
                                              / std::max(1.0f, cellHeight));
        }
        ui.setCursor(Cursor::Hand);
    }

    list.addRectFilled(grip, isSelected ? t.selection.withAlpha(0.35f)
                                        : t.borderStrong.withAlpha(hovered ? 0.6f : 0.3f),
                       2.0f);

    // Resize from the bottom-right corner, the convention everywhere.
    const Rect corner{ cell.right() - t.scaled(12.0f), cell.bottom() - t.scaled(12.0f),
                       t.scaled(12.0f), t.scaled(12.0f) };
    const UiId cornerId = ui.id("control.size." + std::to_string(control.id));

    bool cornerHovered = false, cornerHeld = false;
    ui.buttonBehaviour(cornerId, corner, cornerHovered, cornerHeld);
    if (ui.isActive(cornerId)) {
        dragging_ = control.id;
        selected_ = control.id;
        resizing_ = true;
        ui.setCursor(Cursor::ResizeDiagonal);
    }

    list.addRectFilled(corner.deflated(t.scaled(3.0f)),
                       cornerHovered ? t.text : t.borderStrong, 1.0f);
}

void ControlView::drawInspector(Ui& ui, const Rect& area) {
    const Theme& t = theme();
    DrawList& list = ui.draw();

    list.addRectFilled(area, t.panel);
    list.addRectFilled(Rect{ area.left(), area.top(), 1.0f, area.height }, t.border);

    Rect column = area.deflated(t.smallPadding);
    ui.label(column.removeFromTop(t.scaled(18.0f)), "control", t.textDim, t.fontUiBold);

    control::Control* control = surface_ ? surface_->find(selected_) : nullptr;
    if (!control) {
        ui.labelDim(column.removeFromTop(t.scaled(40.0f)),
                    "select a control, or add one from the toolbar");
        return;
    }

    // -- name --------------------------------------------------------------
    if (nameBufferFor_ != control->id) {
        nameBuffer_ = control->name;
        nameBufferFor_ = control->id;
    }
    if (ui.textField(ui.id("control.name"), column.removeFromTop(t.scaled(22.0f)),
                     nameBuffer_, "name")) {
        control->name = nameBuffer_;
        if (onModified) onModified();
    }

    column.removeFromTop(t.scaled(4.0f));
    ui.labelDim(column.removeFromTop(t.scaled(14.0f)), control::toString(control->kind));

    if (control->kind == control::ControlKind::Button) {
        bool momentary = control->momentary;
        if (ui.checkbox(ui.id("control.momentary"), column.removeFromTop(t.scaled(20.0f)),
                        "momentary", momentary)) {
            control->momentary = momentary;
            if (onModified) onModified();
        }
        if (ui.isHot(ui.id("control.momentary")))
            ui.setTooltip("Returns to off when released, like a build's engage switch");
    }

    if (ui.button(ui.id("control.delete"), column.removeFromTop(t.scaled(20.0f)), "delete",
                  Ui::ButtonStyle::Danger)) {
        surface_->remove(control->id);
        selected_ = 0;
        if (onModified) onModified();
        return;
    }

    if (!control->drivesParameters()) return;

    // -- targets -----------------------------------------------------------
    for (int axis = 0; axis < (control->hasSecondAxis() ? 2 : 1); ++axis) {
        const bool second = axis == 1;
        std::vector<control::Target>& targets = second ? control->targetsY : control->targets;

        ui.separator(column.removeFromTop(t.scaled(9.0f)));

        // Tall enough for a button label with descenders: at 18 units the
        // "waiting" state lost the tail of its g.
        Rect header = column.removeFromTop(t.scaled(22.0f));
        ui.label(header.removeFromLeft(t.scaled(70.0f)),
                 control->hasSecondAxis() ? (second ? "y targets" : "x targets") : "targets",
                 t.textDim, t.fontUiBold);

        const bool waiting = learnControl_ == control->id && learnSecondAxis_ == second;
        if (ui.button(ui.id(second ? "control.learn.y" : "control.learn.x"),
                      header.removeFromRight(t.scaled(62.0f)),
                      waiting ? "waiting" : "learn",
                      waiting ? Ui::ButtonStyle::Primary : Ui::ButtonStyle::Normal, waiting)) {
            if (waiting) {
                cancelLearn();
            } else {
                learnControl_ = control->id;
                learnSecondAxis_ = second;
            }
        }
        if (ui.isHot(ui.id(second ? "control.learn.y" : "control.learn.x"))) {
            ui.setTooltip("Then move the parameter you want, anywhere in the app. "
                          "Where this control is sitting decides which end of its "
                          "range the parameter's current value becomes.");
        }

        column.removeFromTop(t.scaled(3.0f));

        if (targets.empty()) {
            ui.labelDim(column.removeFromTop(t.scaled(16.0f)), "nothing bound yet");
            continue;
        }

        // A copy of the addresses, because removing inside the loop would
        // invalidate the iteration over the vector being drawn from.
        std::vector<ParamAddress> addresses;
        addresses.reserve(targets.size());
        for (const control::Target& target : targets) addresses.push_back(target.address);

        for (const ParamAddress address : addresses) {
            if (column.height < t.scaled(40.0f)) break;

            control::Target* target = nullptr;
            for (control::Target& candidate : targets)
                if (candidate.address == address) { target = &candidate; break; }
            if (!target) continue;

            Rect nameRow = column.removeFromTop(t.scaled(16.0f));
            const Rect removeArea = nameRow.removeFromRight(t.scaled(18.0f));

            const UiId removeId = ui.id("control.unbind."
                                        + std::to_string(address.key())
                                        + (second ? "y" : "x"));
            if (ui.iconButton(removeId, removeArea, Ui::Icon::Cross, t.textFaint)) {
                surface_->unbind(control->id, address, second);
                if (onModified) onModified();
                break;
            }

            list.addTextClipped(ui.font(t.fontSmall), nameRow, t.textDim, describe(address));

            // The two ends of the range, as sliders. Crossing them is allowed:
            // a target whose low is above its high runs backwards, which is how
            // one knob opens a filter while it closes a gate.
            Rect rangeRow = column.removeFromTop(t.scaled(16.0f));
            rangeRow.removeFromLeft(t.scaled(10.0f));

            float low = target->low;
            float high = target->high;

            const Rect lowArea = rangeRow.removeFromLeft(rangeRow.width * 0.5f - t.scaled(2.0f));
            rangeRow.removeFromLeft(t.scaled(4.0f));

            bool changed = false;
            changed |= ui.sliderNormalised(ui.id("control.low." + std::to_string(address.key())),
                                           lowArea, low, t.accentDim);
            changed |= ui.sliderNormalised(ui.id("control.high." + std::to_string(address.key())),
                                           rangeRow, high, t.accent);
            if (changed) {
                surface_->setTargetRange(control->id, address, low, high, second);
                if (onModified) onModified();
            }

            column.removeFromTop(t.scaled(3.0f));
        }
    }
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void ControlView::render(Ui& ui, const Rect& bounds) {
    const Theme& t = theme();
    if (!surface_) return;

    ui.draw().addRectFilled(bounds, t.background);

    Rect area = bounds;
    drawToolbar(ui, area);
    drawPageTabs(ui, area);

    Rect inspector;
    if (editing_) inspector = area.removeFromRight(inspectorWidth());

    const Rect field = area.deflated(t.padding);
    if (editing_) drawGrid(ui, field);

    control::Page* page = surface_->page(surface_->activePage());
    if (page) {
        // Two passes: every control drawn, then every overlay. Otherwise a
        // control drawn later covers the previous one's drag handle, and the
        // last row of a dense layout becomes the only row that can be moved.
        for (control::Control& control : page->controls) {
            drawControl(ui, gridCell(field, control.column, control.row,
                                     control.width, control.height),
                        control);
        }

        if (editing_) {
            for (control::Control& control : page->controls) {
                drawEditOverlay(ui, gridCell(field, control.column, control.row,
                                             control.width, control.height),
                                control);
            }
        }
    }

    // -- drag and resize ---------------------------------------------------
    if (dragging_ != 0) {
        const float cellWidth = field.width / static_cast<float>(std::max(1, surface_->columns()));
        const float cellHeight = field.height / static_cast<float>(std::max(1, surface_->rows()));

        const Vec2 pointer = ui.input().mousePosition;
        const int column = static_cast<int>(std::floor((pointer.x - field.left()) / cellWidth));
        const int row = static_cast<int>(std::floor((pointer.y - field.top()) / cellHeight));

        if (resizing_) {
            if (const control::Control* control = surface_->find(dragging_))
                surface_->resize(dragging_, column - control->column + 1,
                                 row - control->row + 1);
        } else {
            surface_->move(dragging_, column - dragOffsetColumn_, row - dragOffsetRow_);
        }

        if (!ui.input().mouseDown[static_cast<int>(MouseButton::Left)]) {
            dragging_ = 0;
            resizing_ = false;
            if (onModified) onModified();
        }
    }

    // Clicking the empty grid clears the selection, which is how anyone expects
    // to get the inspector back to nothing.
    if (editing_ && ui.hovering(field)
        && ui.input().mousePressed[static_cast<int>(MouseButton::Left)]) {
        selected_ = 0;
    }

    if (editing_) drawInspector(ui, inspector);

    // A learn that is waiting has to say so somewhere the eye is, because the
    // parameter being reached for is on another tab.
    if (learning()) {
        if (!ui.hasNotification())
            ui.notify("touch the parameter to bind - Esc to cancel", t.control, 1.0f);
        if (ui.input().keyPressed(key::Escape)) cancelLearn();
    }
}

} // namespace acm::ui

// The control tab: the surface as played, and the editor that builds it.
//
// Two modes in one view, deliberately. A separate editor window would mean
// building a layout against a picture of it rather than against the thing
// itself, and the only question that matters while laying out a surface - "is
// this knob where my hand expects it" - can only be answered by reaching for it.
// So edit mode adds a grid, handles and a palette on top of the live surface,
// and the controls keep working while it is on.
//
// Binding is by learn rather than by menu. Picking a parameter out of a list of
// several hundred means knowing what it is called; touching it means knowing
// what it does, which is the thing a performer actually knows.
#pragma once

#include "../control/Surface.h"
#include "../core/Engine.h"
#include "../net/ControlServer.h"
#include "MetasurfaceView.h"
#include "Ui.h"

#include <functional>
#include <string>

namespace acm::ui {

class ControlView {
public:
    void initialise(Engine* engine, control::Surface* surface, MetasurfaceView* metasurfaceView,
                    net::ControlServer* server);

    void render(Ui& ui, const Rect& bounds);

    bool editing() const noexcept { return editing_; }
    void setEditing(bool on) noexcept { editing_ = on; if (!on) cancelLearn(); }

    // True while waiting for a parameter to be touched. The application shows
    // this in the status bar and feeds the touched parameter back in.
    bool learning() const noexcept { return learnControl_ != 0; }
    void cancelLearn() noexcept { learnControl_ = 0; }
    // Called by the application when a parameter is touched anywhere in the
    // interface while learning. Returns true when it was taken.
    bool completeLearn(ParamAddress address);

    // Raised whenever the surface changes shape, so the patch is marked dirty.
    std::function<void()> onModified;

private:
    Rect gridCell(const Rect& area, int column, int row, int width, int height) const;

    void drawToolbar(Ui& ui, Rect& area);
    void drawServerRow(Ui& ui, Rect& area);
    void drawPageTabs(Ui& ui, Rect& area);
    void drawGrid(Ui& ui, const Rect& area) const;
    void drawControl(Ui& ui, const Rect& area, control::Control& control);
    void drawEditOverlay(Ui& ui, const Rect& cell, control::Control& control);
    void drawInspector(Ui& ui, const Rect& area);

    // Name of a bound parameter, for the target list: "Deck A . Loop Length".
    std::string describe(ParamAddress address) const;

    Engine* engine_ = nullptr;
    control::Surface* surface_ = nullptr;
    MetasurfaceView* metasurfaceView_ = nullptr;
    // Serving the same surface to a tablet. Owned by the application, because
    // it has to keep running while another tab is in front.
    net::ControlServer* server_ = nullptr;
    int serverPort_ = 8420;

    bool editing_ = false;

    // The control being edited, dragged or resized, and which corner of it.
    int selected_ = 0;
    int dragging_ = 0;
    bool resizing_ = false;
    // Where in the control the drag started, in cells, so it does not jump to
    // put its top-left corner under the pointer.
    int dragOffsetColumn_ = 0;
    int dragOffsetRow_ = 0;

    // The control waiting for a parameter to be touched, and which axis.
    int learnControl_ = 0;
    bool learnSecondAxis_ = false;

    std::string nameBuffer_;
    int nameBufferFor_ = 0;
};

} // namespace acm::ui

// The control surface: the layout a performer actually plays, as opposed to the
// patch that produces the sound.
//
// A patch of any size has hundreds of parameters, and perhaps eight of them
// matter once the set starts. The surface is where those eight go: knobs,
// faders, buttons and X-Y pads placed on a grid, each one driving as many
// parameters as it likes.
//
// Two decisions are worth stating because everything else follows from them.
//
// First, a control binds to a *list* of targets rather than to one parameter,
// and each target carries its own range. That is the whole difference between a
// remote control and a macro: one knob can open a filter from 200 Hz to 8 kHz
// while it also takes a reverb from dry to a third wet and pushes a delay's
// feedback over only the top half of its travel. A single-target binding is
// just the degenerate case.
//
// Second, positions are grid cells rather than pixels. The surface has to be
// laid out once and then look right on the machine it was built on, on a
// projector, and on a tablet over the network - and a pixel layout is correct
// on exactly one of those.
#pragma once

#include "../core/Graph.h"
#include "../core/Json.h"
#include "../core/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acm::control {

enum class ControlKind : int {
    Knob = 0,
    Fader,
    Button,      // toggles by default; momentary is a flag
    XYPad,
    Metasurface, // the 2D snapshot surface, embedded as an element
    Label,       // text only, for marking out sections of the layout
    Count
};

const char* toString(ControlKind kind) noexcept;
ControlKind controlKindFromString(const std::string& text) noexcept;

// One parameter driven by one control, over a slice of its own range.
struct Target {
    ParamAddress address;

    // Where the parameter sits when the control is at 0 and at 1, in the
    // parameter's normalised space. Inverting a target is `low > high`, which
    // is a real thing to want: one knob that opens a filter as it closes a gate.
    float low = 0.0f;
    float high = 1.0f;

    float valueFor(float amount) const noexcept {
        return low + (high - low) * clampValue(amount, 0.0f, 1.0f);
    }
};

struct Control {
    int id = 0;
    ControlKind kind = ControlKind::Knob;
    std::string name;
    std::uint32_t colour = 0;   // 0 = take the theme's accent

    // Grid cell, in columns and rows.
    int column = 0;
    int row = 0;
    int width = 2;
    int height = 2;

    // The value the control is showing, 0..1. For an X-Y pad this is the
    // horizontal axis and `valueY` the vertical.
    float value = 0.0f;
    float valueY = 0.0f;

    // A momentary button returns to 0 when released; a toggle stays put. A
    // build's engage switch has to be the former and a mute the latter, and
    // getting it wrong leaves the build running.
    bool momentary = false;

    // X-Y pads drive two independent lists.
    std::vector<Target> targets;
    std::vector<Target> targetsY;

    bool drivesParameters() const noexcept {
        return kind != ControlKind::Metasurface && kind != ControlKind::Label;
    }
    bool hasSecondAxis() const noexcept { return kind == ControlKind::XYPad; }
};

// A page of controls. Pages exist because a surface built for one song is not
// the surface for the next one, and because eight controls is a layout while
// forty is a menu.
struct Page {
    std::string name;
    std::vector<Control> controls;
};

class Surface {
public:
    Surface();

    // -- pages -------------------------------------------------------------
    const std::vector<Page>& pages() const noexcept { return pages_; }
    int pageCount() const noexcept { return static_cast<int>(pages_.size()); }
    int activePage() const noexcept { return activePage_; }
    void setActivePage(int index) noexcept;

    int addPage(std::string name);
    bool removePage(int index);
    bool renamePage(int index, std::string name);

    Page* page(int index);
    const Page* page(int index) const;

    // -- controls ----------------------------------------------------------
    // All operate on the active page.
    Control* find(int id);
    const Control* find(int id) const;

    // Returns the new control's id, or 0. The cell is taken as given; nothing
    // stops two controls overlapping, because a layout tool that refuses to let
    // you put something down while you rearrange is worse than one that lets you
    // make a mess and tidy it up.
    int add(ControlKind kind, std::string name, int column, int row,
            int width, int height);
    bool remove(int id);
    bool move(int id, int column, int row);
    bool resize(int id, int width, int height);

    // -- grid --------------------------------------------------------------
    int columns() const noexcept { return columns_; }
    int rows() const noexcept { return rows_; }
    void setGrid(int columns, int rows) noexcept;

    // -- driving the graph -------------------------------------------------
    // Sets a control's value and writes just that control. The interactive path:
    // moving one knob should not re-write the whole surface, or two controls
    // sharing a parameter would fight every frame instead of last-touch
    // winning.
    bool setValue(int id, float value, Graph& graph);
    bool setValueXY(int id, float x, float y, Graph& graph);

    // Reads the graph back into a control's targets - the inverse operation.
    void adoptFromGraph(int id, const Graph& graph);

    // The same for every control on the active page. Called when the page
    // changes and after a patch loads.
    //
    // Adopting rather than applying is the deliberate choice. A page that wrote
    // its stored values into the graph on arrival would move parameters the
    // performer had just set from somewhere else - switching pages to look at
    // something would change the sound. Reading instead means a control always
    // shows where its parameter actually is.
    void adoptAllFromGraph(const Graph& graph);

    // -- binding -----------------------------------------------------------
    // Adds a target to a control, taking its current value as one end of the
    // range and leaving the other at the extreme. Which end depends on where
    // the control is sitting: binding a parameter with the knob down means "this
    // is the bottom", and with the knob up means "this is the top".
    bool bind(int id, ParamAddress address, const Graph& graph, bool secondAxis = false);
    bool unbind(int id, ParamAddress address, bool secondAxis = false);
    bool setTargetRange(int id, ParamAddress address, float low, float high,
                        bool secondAxis = false);

    // Drops targets whose node has gone. Called after a node is deleted, for the
    // same reason the metasurface prunes.
    void pruneMissing(const Graph& graph);

    // -- persistence -------------------------------------------------------
    JsonValue toJson() const;
    void fromJson(const JsonValue& in);
    void clear();

private:
    static void applyTargets(const std::vector<Target>& targets, float amount, Graph& graph);

    std::vector<Page> pages_;
    int activePage_ = 0;
    int nextId_ = 1;

    int columns_ = 12;
    int rows_ = 8;
};

} // namespace acm::control

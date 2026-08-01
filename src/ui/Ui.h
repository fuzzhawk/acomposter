// Immediate-mode UI framework.
//
// The whole interface is rebuilt every frame from the application's state, so
// there is no widget tree to keep in sync with the graph and no possibility of
// the display disagreeing with the engine. Interaction is tracked with the usual
// hot/active pair: `hot` is what the pointer is over, `active` is what the
// pointer captured on press and keeps until release, which is what lets a drag
// continue after the pointer leaves the control.
//
// Widgets take an explicit rectangle rather than flowing in a layout, because
// almost every surface here is a canvas, a mixer strip or a meter bridge whose
// geometry is dictated by the thing being shown.
#pragma once

#include "../core/Parameter.h"
#include "../gfx/DrawList.h"
#include "../gfx/FontAtlas.h"
#include "../gfx/Geometry.h"
#include "Theme.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace acm::ui {

using gfx::Colour;
using gfx::DrawList;
using gfx::Rect;
using gfx::Vec2;

using UiId = std::uint64_t;
inline constexpr UiId kNoId = 0;

// Virtual key codes, matching Win32's so the platform layer can pass them
// straight through.
namespace key {
inline constexpr int Backspace = 0x08;
inline constexpr int Tab = 0x09;
inline constexpr int Enter = 0x0D;
inline constexpr int Shift = 0x10;
inline constexpr int Control = 0x11;
inline constexpr int Alt = 0x12;
inline constexpr int Escape = 0x1B;
inline constexpr int Space = 0x20;
inline constexpr int PageUp = 0x21;
inline constexpr int PageDown = 0x22;
inline constexpr int End = 0x23;
inline constexpr int Home = 0x24;
inline constexpr int Left = 0x25;
inline constexpr int Up = 0x26;
inline constexpr int Right = 0x27;
inline constexpr int Down = 0x28;
inline constexpr int Delete = 0x2E;
inline constexpr int A = 'A';
inline constexpr int B = 'B';
inline constexpr int C = 'C';
inline constexpr int D = 'D';
inline constexpr int N = 'N';
inline constexpr int O = 'O';
inline constexpr int R = 'R';
inline constexpr int S = 'S';
inline constexpr int V = 'V';
inline constexpr int X = 'X';
inline constexpr int Z = 'Z';
inline constexpr int F1 = 0x70;
inline constexpr int F12 = 0x7B;
} // namespace key

enum class MouseButton : int { Left = 0, Right = 1, Middle = 2, Count = 3 };

enum class Cursor : int {
    Arrow, Hand, ResizeHorizontal, ResizeVertical, ResizeDiagonal, Text, Crosshair, NotAllowed
};

// Everything the platform layer collects between frames.
struct InputState {
    Vec2 mousePosition{ 0.0f, 0.0f };
    Vec2 mouseDelta{ 0.0f, 0.0f };
    float wheel = 0.0f;
    float wheelHorizontal = 0.0f;

    bool mouseDown[3] = {};
    bool mousePressed[3] = {};
    bool mouseReleased[3] = {};
    bool mouseDoubleClicked[3] = {};

    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    // Code points typed this frame, already filtered of control characters.
    std::vector<std::uint32_t> textInput;

    struct KeyEvent {
        int code = 0;
        bool pressed = false;
        bool repeat = false;
    };
    std::vector<KeyEvent> keyEvents;
    bool keyHeld[256] = {};

    // Files dropped on the window this frame, as UTF-8 paths.
    std::vector<std::string> droppedFiles;
    Vec2 dropPosition{ 0.0f, 0.0f };

    bool windowFocused = true;

    bool keyPressed(int code) const;
    bool keyDown(int code) const { return code >= 0 && code < 256 && keyHeld[code]; }
    void clearPerFrame();
};

class Ui {
public:
    Ui();

    void setFontAtlas(const gfx::FontAtlas* atlas) { atlas_ = atlas; }
    const gfx::Font& font(gfx::FontId id) const;

    void beginFrame(const InputState& input, Vec2 displaySize, float deltaSeconds);
    void endFrame();

    DrawList& draw() noexcept { return drawList_; }
    // Rendered after the main list: menus, tooltips, drag ghosts.
    DrawList& overlay() noexcept { return overlayList_; }
    const InputState& input() const noexcept { return input_; }
    float deltaSeconds() const noexcept { return deltaSeconds_; }
    Vec2 displaySize() const noexcept { return displaySize_; }

    // -- identifiers -------------------------------------------------------
    UiId id(std::string_view label) const;
    UiId idFrom(const void* pointer, int index = 0) const;
    void pushId(std::string_view label);
    void pushId(std::uint64_t value);
    void popId();

    // -- interaction state -------------------------------------------------
    UiId hot() const noexcept { return hot_; }
    UiId active() const noexcept { return active_; }
    bool isActive(UiId control) const noexcept { return active_ == control; }
    bool isHot(UiId control) const noexcept { return hot_ == control; }
    void setActive(UiId control) noexcept { active_ = control; }
    void clearActive() noexcept { active_ = kNoId; }

    // True when some widget has captured the pointer, so a background view
    // should not also start a drag.
    bool pointerCaptured() const noexcept { return active_ != kNoId; }
    // Where the pointer was when the active control captured it, which is what
    // a range drag needs to know its anchor.
    Vec2 dragStart() const noexcept { return dragStartPosition_; }
    bool keyboardCaptured() const noexcept { return editing_ != kNoId; }

    void setCursor(Cursor cursor) noexcept { cursor_ = cursor; }
    Cursor cursor() const noexcept { return cursor_; }

    // -- hit testing -------------------------------------------------------
    // Respects the clip stack, so a control scrolled out of a panel is not hit.
    bool hovering(const Rect& rect) const;
    // The standard press/hold/release cycle. `outHeld` stays true while the
    // button is down even if the pointer leaves; the return value is the click.
    bool buttonBehaviour(UiId control, const Rect& rect, bool& outHovered, bool& outHeld);

    void pushClip(const Rect& rect);
    void popClip();

    // -- primitives --------------------------------------------------------
    void panel(const Rect& rect, bool raised = false);
    void separator(const Rect& rect, bool vertical = false);
    void label(const Rect& rect, std::string_view text, const Colour& colour,
               gfx::FontId font, DrawList::Align align = DrawList::Align::Left);
    void labelDim(const Rect& rect, std::string_view text,
                  DrawList::Align align = DrawList::Align::Left);

    // -- widgets -----------------------------------------------------------
    enum class ButtonStyle { Normal, Primary, Ghost, Danger, Toggle };

    bool button(UiId control, const Rect& rect, std::string_view label,
                ButtonStyle style = ButtonStyle::Normal, bool selected = false,
                bool enabled = true);

    // A button that draws a shape rather than a glyph, so the interface needs no
    // icon font and no image assets.
    enum class Icon {
        Play, Stop, Pause, Record, Loop, Rewind, Plus, Minus, Cross, Chevron,
        Gear, Folder, Save, Power, Wave, Grid, Target, Link, Trash, Refresh,
    };
    bool iconButton(UiId control, const Rect& rect, Icon icon,
                    const Colour& colour, bool selected = false, bool enabled = true);
    void drawIcon(DrawList& list, const Rect& rect, Icon icon, const Colour& colour);

    bool checkbox(UiId control, const Rect& rect, std::string_view label, bool& value);

    // Normalised 0..1 slider. `outChanged` distinguishes a drag from a release.
    bool sliderNormalised(UiId control, const Rect& rect, float& value,
                          const Colour& fill, bool vertical = false);

    // Rotary control. Vertical drag, fine with Shift, reset with double click.
    bool knob(UiId control, const Rect& rect, float& normalised, const Colour& fill,
              float defaultValue = 0.5f);

    // The parameter-aware wrappers everything in the app actually calls: they
    // read and write the Parameter directly and show its formatted value.
    // The parameter a widget was last moved. Reset at the start of each frame.
    //
    // This exists for the control surface's learn mode, which has to know which
    // parameter the performer just touched. Reporting it here means no widget
    // has to know that learn exists - the alternative was a "learning" flag
    // threaded through every panel that draws a parameter, which is all of them.
    const Parameter* touchedParameter() const noexcept { return touchedParameter_; }

    bool parameterKnob(const Rect& rect, Parameter& parameter, const Colour& fill,
                       bool showLabel = true);
    bool parameterSlider(const Rect& rect, Parameter& parameter, const Colour& fill,
                         bool showLabel = true);
    bool parameterToggle(const Rect& rect, Parameter& parameter, const Colour& fill);
    bool parameterChoice(const Rect& rect, Parameter& parameter);

    // A choice that advances on click rather than opening a menu. Right-click
    // or shift-click steps back.
    //
    // Inside a node body this is the better control by some distance: the menu
    // a combo opens falls outside the node, over the canvas, and has to be
    // aimed at while the set is running. One click on a control that is already
    // under the finger beats hitting a three-pixel row on a touchscreen.
    bool parameterCycle(const Rect& rect, Parameter& parameter, const Colour& accent);

    // A bare integer field, dragged horizontally or typed into. Used for things
    // that are not node parameters - bar numbers, counts - and so cannot go
    // through the parameter widgets.
    bool intField(UiId control, const Rect& rect, int& value, int lo, int hi);
    // One row: name on the left, an editable value on the right.
    bool parameterRow(const Rect& rect, Parameter& parameter, const Colour& fill);

    // Text entry. Returns true when the value was committed with Enter or focus
    // loss; `text` is updated live while editing.
    bool textField(UiId control, const Rect& rect, std::string& text,
                   std::string_view placeholder = {}, bool selectAllOnFocus = false);
    bool editingText(UiId control) const noexcept { return editing_ == control; }
    void beginTextEdit(UiId control, const std::string& initial, bool selectAll);
    void cancelTextEdit();

    // -- modality ----------------------------------------------------------
    // A modal sheet is drawn on top of everything, which in an immediate-mode
    // interface means it is drawn *last* - by which point every view underneath
    // has already had the frame's input and taken it. Telling the Ui a modal is
    // up, before any of them run, is what stops that: hovering fails everywhere
    // except between beginModal and endModal.
    void setModalActive(bool active) noexcept { modalActive_ = active; }
    bool modalActive() const noexcept { return modalActive_; }
    void beginModal() noexcept { insideModal_ = true; }
    void endModal() noexcept { insideModal_ = false; }

    // Bounds popups to a rectangle rather than to the window. A modal sheet
    // sets this to itself so its dropdowns stay inside it and scroll, instead
    // of hanging out over the canvas where they read as belonging to neither.
    void setPopupContainer(const Rect& r) noexcept { popupContainer_ = r; hasPopupContainer_ = true; }
    void clearPopupContainer() noexcept { hasPopupContainer_ = false; }

    // -- popups ------------------------------------------------------------
    // A popup opened this frame is drawn in the overlay next frame onwards,
    // above everything, and closes on click-outside or Escape.
    void openPopup(UiId control, Vec2 anchor, Vec2 size);
    bool popupOpen(UiId control) const noexcept { return popupId_ == control; }
    // Returns the popup's rect when it is open; draw into overlay() inside.
    bool beginPopup(UiId control, Rect& outRect);
    void endPopup();
    void closePopup();
    UiId currentPopup() const noexcept { return popupId_; }

    // A vertical list of choices inside a popup. Returns the chosen index, or -1.
    int popupMenu(UiId control, const Rect& rect, const std::vector<std::string>& items,
                  int selected = -1, const std::vector<bool>* enabled = nullptr);

    // Dropdown: a button showing the current choice that opens a menu.
    bool combo(UiId control, const Rect& rect, const std::vector<std::string>& items,
               int& selectedIndex);

    // -- scrolling ---------------------------------------------------------
    // Clips to `rect` and offsets everything drawn inside by the scroll
    // position. Returns the inner rect in content coordinates.
    Rect beginScroll(UiId control, const Rect& rect, float contentHeight);
    void endScroll();
    float scrollOffset(UiId control) const;
    void setScrollOffset(UiId control, float offset);

    // -- meters ------------------------------------------------------------
    void meter(const Rect& rect, float level, bool vertical = true);
    void stereoMeter(const Rect& rect, float left, float right, bool vertical = true);

    // -- feedback ----------------------------------------------------------
    void setTooltip(std::string text);
    // A transient message strip; used for save confirmations and load warnings.
    void notify(std::string text, const Colour& colour, float seconds = 4.0f);
    bool hasNotification() const noexcept { return notificationTimer_ > 0.0f; }

    // -- drag payload ------------------------------------------------------
    // Lightweight typed drag-and-drop between views (a plugin from the browser
    // onto the canvas, a looper take onto a sample player).
    void beginDrag(std::string type, std::string payload);
    bool dragging() const noexcept { return !dragType_.empty(); }
    const std::string& dragType() const noexcept { return dragType_; }
    const std::string& dragPayload() const noexcept { return dragPayload_; }
    // True on the frame the drag is released over `rect`.
    bool acceptDrop(const Rect& rect, std::string_view type);
    void cancelDrag();

private:
    struct TextEditState {
        std::string buffer;
        std::size_t cursor = 0;
        std::size_t selectionAnchor = 0;
        float scrollX = 0.0f;
        bool hasSelection() const { return cursor != selectionAnchor; }
        void clearSelection() { selectionAnchor = cursor; }
    };

    void drawTooltip();
    void drawNotification();
    void updateTextEdit(const Rect& rect, const gfx::Font& font);
    void deleteSelection();
    void insertText(std::string_view utf8);

    UiId hashId(std::string_view label) const;

    const gfx::FontAtlas* atlas_ = nullptr;

    DrawList drawList_;
    DrawList overlayList_;
    InputState input_;
    Vec2 displaySize_{ 0.0f, 0.0f };
    float deltaSeconds_ = 1.0f / 60.0f;

    UiId hot_ = kNoId;
    UiId hotNext_ = kNoId;
    UiId active_ = kNoId;
    UiId editing_ = kNoId;
    Cursor cursor_ = Cursor::Arrow;

    std::vector<UiId> idStack_;

    // Drag state for sliders and knobs: the value where the grab started, so a
    // drag is always relative and never jumps.
    float dragStartValue_ = 0.0f;
    Vec2 dragStartPosition_{ 0.0f, 0.0f };

    TextEditState textEdit_;

    UiId popupId_ = kNoId;
    Rect popupRect_;
    bool popupJustOpened_ = false;
    bool insidePopup_ = false;
    Rect popupContainer_{};
    bool hasPopupContainer_ = false;
    // Sub-step remainder while an integer field is being dragged.
    float dragAccumulator_ = 0.0f;
    bool modalActive_ = false;
    bool insideModal_ = false;

    struct ScrollState {
        UiId id = kNoId;
        float offset = 0.0f;
        float contentHeight = 0.0f;
        Rect viewRect;
    };
    std::vector<ScrollState> scrollStates_;
    std::vector<UiId> scrollStack_;

    std::string tooltip_;
    Vec2 tooltipPosition_{ 0.0f, 0.0f };

    // Set by the parameter widgets, cleared each frame. See touchedParameter().
    const Parameter* touchedParameter_ = nullptr;

    std::string notification_;
    Colour notificationColour_;
    float notificationTimer_ = 0.0f;

    std::string dragType_;
    std::string dragPayload_;
    bool dragReleasedThisFrame_ = false;
};

} // namespace acm::ui

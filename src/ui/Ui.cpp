#include "Ui.h"

#include "../core/Utf.h"

#include <algorithm>
#include <cmath>

namespace acm::ui {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Knob sweep: 270 degrees, leaving a gap at the bottom so the value's extent is
// unambiguous at a glance.
constexpr float kKnobStartAngle = kPi * 0.75f;
constexpr float kKnobSweep = kPi * 1.5f;

// Pixels of vertical drag for the full range of a knob.
constexpr float kKnobDragRange = 220.0f;
constexpr float kFineDragMultiplier = 0.2f;

std::uint64_t fnv1a(std::string_view text, std::uint64_t seed) {
    std::uint64_t hash = seed ? seed : 0xCBF29CE484222325ull;
    for (char c : text) {
        hash ^= static_cast<std::uint8_t>(c);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xC0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

} // namespace

// ---------------------------------------------------------------------------
// InputState
// ---------------------------------------------------------------------------

bool InputState::keyPressed(int code) const {
    for (const KeyEvent& event : keyEvents)
        if (event.code == code && event.pressed) return true;
    return false;
}

void InputState::clearPerFrame() {
    for (int i = 0; i < 3; ++i) {
        mousePressed[i] = false;
        mouseReleased[i] = false;
        mouseDoubleClicked[i] = false;
    }
    wheel = 0.0f;
    wheelHorizontal = 0.0f;
    mouseDelta = { 0.0f, 0.0f };
    textInput.clear();
    keyEvents.clear();
    droppedFiles.clear();
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

Ui::Ui() {
    idStack_.reserve(16);
    scrollStates_.reserve(32);
}

const gfx::Font& Ui::font(gfx::FontId id) const {
    static const gfx::Font empty;
    return atlas_ ? atlas_->font(id) : empty;
}

void Ui::beginFrame(const InputState& input, Vec2 displaySize, float deltaSeconds) {
    input_ = input;
    displaySize_ = displaySize;
    deltaSeconds_ = deltaSeconds;

    drawList_.beginFrame(displaySize);
    overlayList_.beginFrame(displaySize);

    hot_ = hotNext_;
    hotNext_ = kNoId;
    cursor_ = Cursor::Arrow;
    tooltip_.clear();
    idStack_.clear();
    scrollStack_.clear();
    insidePopup_ = false;

    // The capture is deliberately NOT cleared here. A widget only learns it has
    // been clicked by seeing, on the release frame, that it is still the active
    // control - so clearing it before the widgets run would eat every click
    // whose press and release fell in different frames. That is nearly all of
    // them: a human click is ~80 ms, which is five frames at 60 Hz. endFrame()
    // clears it instead, once everyone has had their chance to see it.

    // Escape abandons text editing without committing.
    if (editing_ != kNoId && input_.keyPressed(key::Escape))
        cancelTextEdit();

    dragReleasedThisFrame_ = false;
    if (dragging() && input_.mouseReleased[static_cast<int>(MouseButton::Left)])
        dragReleasedThisFrame_ = true;

    if (notificationTimer_ > 0.0f) notificationTimer_ -= deltaSeconds;

    if (popupJustOpened_) popupJustOpened_ = false;
}

void Ui::endFrame() {
    // Release the capture now that every widget has had a chance to claim the
    // release. Anything still holding it either handled the click itself or has
    // gone away; either way the pointer is free again.
    if (input_.mouseReleased[static_cast<int>(MouseButton::Left)])
        active_ = kNoId;

    // A click that reached no widget dismisses the popup.
    if (popupId_ != kNoId && !popupJustOpened_
        && (input_.mousePressed[static_cast<int>(MouseButton::Left)]
            || input_.mousePressed[static_cast<int>(MouseButton::Right)])) {
        if (!popupRect_.contains(input_.mousePosition)) closePopup();
    }

    if (popupId_ != kNoId && input_.keyPressed(key::Escape)) closePopup();

    // Every drag ends on the release, accepted or not - and only here, so that
    // whoever accepted it has already read the payload. Keying this off the
    // released flag alone would leave an accepted drop's payload behind, since
    // acceptDrop clears that flag to stop a second target taking the same drop.
    if (input_.mouseReleased[static_cast<int>(MouseButton::Left)]) cancelDrag();

    drawNotification();
    drawTooltip();
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

UiId Ui::hashId(std::string_view label) const {
    const std::uint64_t seed = idStack_.empty() ? 0 : idStack_.back();
    const std::uint64_t hash = fnv1a(label, seed);
    // Zero is the "no control" sentinel, so never return it.
    return hash == kNoId ? 1 : hash;
}

UiId Ui::id(std::string_view label) const { return hashId(label); }

UiId Ui::idFrom(const void* pointer, int index) const {
    const std::uint64_t seed = idStack_.empty() ? 0xCBF29CE484222325ull : idStack_.back();
    std::uint64_t hash = seed ^ reinterpret_cast<std::uintptr_t>(pointer);
    hash *= 0x100000001B3ull;
    hash ^= static_cast<std::uint64_t>(index + 1);
    hash *= 0x100000001B3ull;
    return hash == kNoId ? 1 : hash;
}

void Ui::pushId(std::string_view label) { idStack_.push_back(hashId(label)); }

void Ui::pushId(std::uint64_t value) {
    const std::uint64_t seed = idStack_.empty() ? 0xCBF29CE484222325ull : idStack_.back();
    idStack_.push_back((seed ^ value) * 0x100000001B3ull);
}

void Ui::popId() {
    if (!idStack_.empty()) idStack_.pop_back();
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

void Ui::pushClip(const Rect& rect) { drawList_.pushClip(rect); }
void Ui::popClip() { drawList_.popClip(); }

bool Ui::hovering(const Rect& rect) const {
    if (!input_.windowFocused && active_ == kNoId) return false;

    // An open popup swallows hovering everywhere except inside itself.
    if (popupId_ != kNoId && !insidePopup_) return false;

    // So does a modal sheet - except for the popups it opens itself, which are
    // drawn in the overlay and are logically part of it.
    if (modalActive_ && !insideModal_ && !insidePopup_) return false;

    const Vec2 p = input_.mousePosition;
    if (!rect.contains(p)) return false;

    // Respect whichever clip stack we are currently drawing into.
    const Rect clip = insidePopup_ ? overlayList_.currentClip() : drawList_.currentClip();
    return clip.contains(p);
}

bool Ui::buttonBehaviour(UiId control, const Rect& rect, bool& outHovered, bool& outHeld) {
    const bool over = hovering(rect);
    outHovered = over;
    outHeld = false;

    if (over && active_ == kNoId) hotNext_ = control;

    if (over && input_.mousePressed[static_cast<int>(MouseButton::Left)] && active_ == kNoId) {
        active_ = control;
        dragStartPosition_ = input_.mousePosition;

        // One widget per press. Holding `active_` is enough to keep later
        // widgets out while the button is down, but a press and its release
        // arriving in the same frame - a touchpad tap, a remote desktop, a
        // synthetic click - releases `active_` before the frame is over, and
        // whatever is drawn underneath would then claim the same press. The
        // input is our own copy, so consuming it here is the whole fix.
        input_.mousePressed[static_cast<int>(MouseButton::Left)] = false;
    }

    if (active_ == control) {
        outHeld = true;
        hotNext_ = control;

        // The click only counts if the pointer is still over the control, which
        // is the standard escape hatch for a mis-click.
        if (input_.mouseReleased[static_cast<int>(MouseButton::Left)]) {
            active_ = kNoId;
            return over;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void Ui::panel(const Rect& rect, bool raised) {
    const Theme& t = theme();
    drawList_.addRectFilled(rect, raised ? t.panelRaised : t.panel, t.cornerRadius);
    drawList_.addRect(rect, t.border, t.borderWidth, t.cornerRadius);
}

void Ui::separator(const Rect& rect, bool vertical) {
    const Theme& t = theme();
    if (vertical) {
        const float x = std::floor(rect.centre().x);
        drawList_.addRectFilled(Rect{ x, rect.top(), 1.0f, rect.height }, t.borderFaint);
    } else {
        const float y = std::floor(rect.centre().y);
        drawList_.addRectFilled(Rect{ rect.left(), y, rect.width, 1.0f }, t.borderFaint);
    }
}

void Ui::label(const Rect& rect, std::string_view text, const Colour& colour,
               gfx::FontId fontId, DrawList::Align align) {
    drawList_.addTextClipped(font(fontId), rect, colour, text, align);
}

void Ui::labelDim(const Rect& rect, std::string_view text, DrawList::Align align) {
    label(rect, text, theme().textDim, theme().fontSmall, align);
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------

bool Ui::button(UiId control, const Rect& rect, std::string_view text,
                ButtonStyle style, bool selected, bool enabled) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    bool hovered = false, held = false;
    const bool clicked = enabled && buttonBehaviour(control, rect, hovered, held);
    if (!enabled) { hovered = false; held = false; }

    Colour fill = t.widgetBackground;
    Colour textColour = enabled ? t.text : t.textFaint;
    Colour borderColour = t.border;

    switch (style) {
        case ButtonStyle::Primary:
            fill = selected || held ? t.accent : t.accentDim;
            textColour = enabled ? t.textOnAccent : t.textFaint;
            borderColour = t.accent;
            break;

        case ButtonStyle::Danger:
            fill = held ? t.danger : t.widgetBackground;
            textColour = held ? t.textOnAccent : t.danger;
            borderColour = t.danger.withAlpha(0.5f);
            break;

        case ButtonStyle::Ghost:
            fill = Colour{ 0.0f, 0.0f, 0.0f, 0.0f };
            borderColour = Colour{ 0.0f, 0.0f, 0.0f, 0.0f };
            break;

        case ButtonStyle::Toggle:
            fill = selected ? t.accentDim : t.widgetBackground;
            textColour = selected ? t.accent : (enabled ? t.textDim : t.textFaint);
            borderColour = selected ? t.accent.withAlpha(0.6f) : t.border;
            break;

        case ButtonStyle::Normal:
            if (selected) { fill = t.widgetActive; borderColour = t.borderStrong; }
            break;
    }

    if (enabled && hovered && style != ButtonStyle::Primary)
        fill = style == ButtonStyle::Ghost ? t.widgetHover.withAlpha(0.6f) : fill.brightened(1.35f);
    if (enabled && held)
        fill = fill.brightened(0.8f);

    if (fill.a > 0.0f) list.addRectFilled(rect, fill, t.cornerRadius);
    if (borderColour.a > 0.0f) list.addRect(rect, borderColour, t.borderWidth, t.cornerRadius);

    // A pressed primary button gets a halo: at a glance across a dark room, the
    // fill alone is not enough of a state change.
    if (style == ButtonStyle::Primary && selected)
        list.addGlow(rect, t.accent.withAlpha(0.35f), 6.0f, t.cornerRadius, 4);

    list.addTextClipped(font(t.fontUi), rect.deflated(t.smallPadding), textColour, text,
                        DrawList::Align::Centre);

    if (enabled && hovered) setCursor(Cursor::Hand);
    return clicked;
}

void Ui::drawIcon(DrawList& list, const Rect& rect, Icon icon, const Colour& colour) {
    // Icons are drawn from primitives rather than a font or bitmaps, so they
    // stay crisp at any DPI and the application ships no image assets.
    const Vec2 c = rect.centre();
    const float s = std::min(rect.width, rect.height) * 0.5f;
    const float thin = std::max(1.0f, s * 0.16f);

    switch (icon) {
        case Icon::Play:
            list.addTriangleFilled({ c.x - s * 0.45f, c.y - s * 0.62f },
                                   { c.x - s * 0.45f, c.y + s * 0.62f },
                                   { c.x + s * 0.62f, c.y }, colour);
            break;

        case Icon::Stop:
            list.addRectFilled(Rect{ c.x - s * 0.52f, c.y - s * 0.52f, s * 1.04f, s * 1.04f },
                               colour, 1.0f);
            break;

        case Icon::Pause:
            list.addRectFilled(Rect{ c.x - s * 0.5f, c.y - s * 0.6f, s * 0.32f, s * 1.2f }, colour, 1.0f);
            list.addRectFilled(Rect{ c.x + s * 0.18f, c.y - s * 0.6f, s * 0.32f, s * 1.2f }, colour, 1.0f);
            break;

        case Icon::Record:
            list.addCircleFilled(c, s * 0.58f, colour);
            break;

        case Icon::Loop: {
            list.addArc(c, s * 0.6f, kPi * 0.25f, kPi * 1.85f, colour, thin);
            // Arrow head closing the loop.
            const Vec2 tip{ c.x + s * 0.6f * std::cos(kPi * 0.25f),
                            c.y + s * 0.6f * std::sin(kPi * 0.25f) };
            list.addTriangleFilled(tip + Vec2{ -s * 0.05f, -s * 0.32f },
                                   tip + Vec2{ s * 0.3f, -s * 0.02f },
                                   tip + Vec2{ -s * 0.22f, s * 0.16f }, colour);
            break;
        }

        case Icon::Rewind:
            list.addTriangleFilled({ c.x + s * 0.15f, c.y - s * 0.55f },
                                   { c.x + s * 0.15f, c.y + s * 0.55f },
                                   { c.x - s * 0.5f, c.y }, colour);
            list.addRectFilled(Rect{ c.x - s * 0.62f, c.y - s * 0.55f, thin, s * 1.1f }, colour);
            break;

        case Icon::Plus:
            list.addRectFilled(Rect{ c.x - s * 0.6f, c.y - thin * 0.5f, s * 1.2f, thin }, colour);
            list.addRectFilled(Rect{ c.x - thin * 0.5f, c.y - s * 0.6f, thin, s * 1.2f }, colour);
            break;

        case Icon::Minus:
            list.addRectFilled(Rect{ c.x - s * 0.6f, c.y - thin * 0.5f, s * 1.2f, thin }, colour);
            break;

        case Icon::Cross:
            list.addLine({ c.x - s * 0.45f, c.y - s * 0.45f }, { c.x + s * 0.45f, c.y + s * 0.45f },
                         colour, thin);
            list.addLine({ c.x + s * 0.45f, c.y - s * 0.45f }, { c.x - s * 0.45f, c.y + s * 0.45f },
                         colour, thin);
            break;

        case Icon::Chevron:
            list.addLine({ c.x - s * 0.4f, c.y - s * 0.2f }, { c.x, c.y + s * 0.25f }, colour, thin);
            list.addLine({ c.x, c.y + s * 0.25f }, { c.x + s * 0.4f, c.y - s * 0.2f }, colour, thin);
            break;

        case Icon::Gear: {
            list.addCircle(c, s * 0.42f, colour, thin);
            for (int i = 0; i < 6; ++i) {
                const float angle = kPi * 2.0f * static_cast<float>(i) / 6.0f;
                const Vec2 direction{ std::cos(angle), std::sin(angle) };
                list.addLine(c + direction * (s * 0.5f), c + direction * (s * 0.78f), colour, thin);
            }
            break;
        }

        case Icon::Folder:
            list.addRectFilled(Rect{ c.x - s * 0.65f, c.y - s * 0.35f, s * 1.3f, s * 0.85f },
                               colour, 1.5f);
            list.addRectFilled(Rect{ c.x - s * 0.65f, c.y - s * 0.55f, s * 0.55f, s * 0.25f },
                               colour, 1.0f);
            break;

        case Icon::Save:
            list.addRect(Rect{ c.x - s * 0.6f, c.y - s * 0.6f, s * 1.2f, s * 1.2f }, colour, thin, 1.5f);
            list.addRectFilled(Rect{ c.x - s * 0.3f, c.y - s * 0.6f, s * 0.6f, s * 0.45f }, colour);
            list.addRectFilled(Rect{ c.x - s * 0.35f, c.y + s * 0.05f, s * 0.7f, s * 0.4f }, colour);
            break;

        case Icon::Power:
            list.addArc(c, s * 0.55f, -kPi * 0.35f, kPi * 1.35f, colour, thin);
            list.addLine({ c.x, c.y - s * 0.7f }, { c.x, c.y - s * 0.05f }, colour, thin);
            break;

        case Icon::Wave: {
            // A little waveform, used for anything sample related.
            Vec2 points[9];
            for (int i = 0; i < 9; ++i) {
                const float t = static_cast<float>(i) / 8.0f;
                points[i] = { rect.left() + rect.width * t,
                              c.y + std::sin(t * kPi * 3.0f) * s * 0.5f };
            }
            list.addPolyline(points, 9, colour, false, thin);
            break;
        }

        case Icon::Grid:
            for (int i = 0; i <= 2; ++i) {
                const float offset = -s * 0.6f + s * 0.6f * static_cast<float>(i);
                list.addRectFilled(Rect{ c.x - s * 0.6f, c.y + offset, s * 1.2f, 1.0f }, colour);
                list.addRectFilled(Rect{ c.x + offset, c.y - s * 0.6f, 1.0f, s * 1.2f }, colour);
            }
            break;

        case Icon::Target:
            list.addCircle(c, s * 0.62f, colour, thin);
            list.addCircleFilled(c, s * 0.2f, colour);
            break;

        case Icon::Link:
            list.addArc({ c.x - s * 0.25f, c.y }, s * 0.35f, kPi * 0.5f, kPi * 1.5f, colour, thin);
            list.addArc({ c.x + s * 0.25f, c.y }, s * 0.35f, -kPi * 0.5f, kPi * 0.5f, colour, thin);
            list.addLine({ c.x - s * 0.2f, c.y }, { c.x + s * 0.2f, c.y }, colour, thin);
            break;

        case Icon::Trash:
            list.addRectFilled(Rect{ c.x - s * 0.45f, c.y - s * 0.35f, s * 0.9f, s * 0.95f },
                               colour, 1.5f);
            list.addRectFilled(Rect{ c.x - s * 0.6f, c.y - s * 0.5f, s * 1.2f, thin }, colour);
            break;

        case Icon::Refresh: {
            list.addArc(c, s * 0.55f, kPi * 0.4f, kPi * 1.9f, colour, thin);
            const Vec2 tip{ c.x + s * 0.55f * std::cos(kPi * 0.4f),
                            c.y + s * 0.55f * std::sin(kPi * 0.4f) };
            list.addTriangleFilled(tip + Vec2{ -s * 0.28f, 0.0f },
                                   tip + Vec2{ s * 0.1f, -s * 0.2f },
                                   tip + Vec2{ s * 0.1f, s * 0.22f }, colour);
            break;
        }
    }
}

bool Ui::iconButton(UiId control, const Rect& rect, Icon icon, const Colour& colour,
                    bool selected, bool enabled) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    bool hovered = false, held = false;
    const bool clicked = enabled && buttonBehaviour(control, rect, hovered, held);

    if (selected) {
        list.addRectFilled(rect, colour.withAlpha(0.18f), t.cornerRadius);
        list.addRect(rect, colour.withAlpha(0.7f), t.borderWidth, t.cornerRadius);
    } else if (enabled && hovered) {
        list.addRectFilled(rect, t.widgetHover, t.cornerRadius);
    }

    Colour iconColour = enabled ? colour : colour.withAlpha(0.3f);
    if (held) iconColour = iconColour.brightened(0.75f);
    else if (enabled && hovered) iconColour = iconColour.brightened(1.2f);

    drawIcon(list, rect.deflated(std::min(rect.width, rect.height) * 0.24f), icon, iconColour);

    if (enabled && hovered) setCursor(Cursor::Hand);
    return clicked;
}

bool Ui::checkbox(UiId control, const Rect& rect, std::string_view text, bool& value) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    bool hovered = false, held = false;
    const bool clicked = buttonBehaviour(control, rect, hovered, held);
    if (clicked) value = !value;

    const float boxSize = std::min(rect.height - 4.0f, 15.0f);
    const Rect box{ rect.left(), rect.centre().y - boxSize * 0.5f, boxSize, boxSize };

    list.addRectFilled(box, value ? t.accentDim : t.widgetBackground, 2.0f);
    list.addRect(box, hovered ? t.accent : (value ? t.accent.withAlpha(0.7f) : t.border),
                 t.borderWidth, 2.0f);

    if (value) {
        // A tick, drawn as two strokes.
        const Vec2 c = box.centre();
        const float s = boxSize * 0.5f;
        list.addLine({ c.x - s * 0.45f, c.y }, { c.x - s * 0.1f, c.y + s * 0.4f }, t.accent, 1.8f);
        list.addLine({ c.x - s * 0.1f, c.y + s * 0.4f }, { c.x + s * 0.5f, c.y - s * 0.42f },
                     t.accent, 1.8f);
    }

    if (!text.empty()) {
        Rect textRect = rect;
        textRect.removeFromLeft(boxSize + t.smallPadding * 1.5f);
        list.addTextClipped(font(t.fontUi), textRect, hovered ? t.text : t.textDim, text);
    }

    if (hovered) setCursor(Cursor::Hand);
    return clicked;
}

// ---------------------------------------------------------------------------
// Sliders and knobs
// ---------------------------------------------------------------------------

bool Ui::sliderNormalised(UiId control, const Rect& rect, float& value,
                          const Colour& fill, bool vertical) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    const bool over = hovering(rect);
    if (over && active_ == kNoId) hotNext_ = control;

    bool changed = false;

    if (over && input_.mousePressed[static_cast<int>(MouseButton::Left)] && active_ == kNoId) {
        active_ = control;
        dragStartValue_ = value;
        dragStartPosition_ = input_.mousePosition;

        // Clicking the track jumps straight to that position; a fader you have
        // to nudge into place is no use in a performance.
        if (!input_.ctrl) {
            const float t01 = vertical
                ? 1.0f - (input_.mousePosition.y - rect.top()) / std::max(1.0f, rect.height)
                : (input_.mousePosition.x - rect.left()) / std::max(1.0f, rect.width);
            value = clampValue(t01, 0.0f, 1.0f);
            dragStartValue_ = value;
            changed = true;
        }
    }

    if (active_ == control) {
        hotNext_ = control;
        const float span = vertical ? rect.height : rect.width;
        const float delta = vertical
            ? -(input_.mousePosition.y - dragStartPosition_.y)
            : (input_.mousePosition.x - dragStartPosition_.x);

        const float scale = input_.shift ? kFineDragMultiplier : 1.0f;
        const float updated = clampValue(dragStartValue_ + (delta / std::max(1.0f, span)) * scale,
                                         0.0f, 1.0f);
        if (updated != value) { value = updated; changed = true; }

        setCursor(vertical ? Cursor::ResizeVertical : Cursor::ResizeHorizontal);
    } else if (over) {
        setCursor(Cursor::Hand);
    }

    if (over && input_.mouseDoubleClicked[static_cast<int>(MouseButton::Left)]) {
        value = 0.5f;
        changed = true;
    }

    // Track.
    list.addRectFilled(rect, t.widgetTrack, t.cornerRadius);

    // Fill.
    const float amount = clampValue(value, 0.0f, 1.0f);
    if (vertical) {
        const float height = rect.height * amount;
        if (height > 0.5f) {
            list.addRectFilled(Rect{ rect.left(), rect.bottom() - height, rect.width, height },
                               fill, t.cornerRadius);
        }
    } else {
        const float width = rect.width * amount;
        if (width > 0.5f)
            list.addRectFilled(Rect{ rect.left(), rect.top(), width, rect.height }, fill, t.cornerRadius);
    }

    list.addRect(rect, (over || active_ == control) ? t.borderStrong : t.border,
                 t.borderWidth, t.cornerRadius);
    return changed;
}

bool Ui::knob(UiId control, const Rect& rect, float& normalised, const Colour& fill,
              float defaultValue) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    const float radius = std::min(rect.width, rect.height) * 0.5f - 2.0f;
    const Vec2 centre = rect.centre();

    const bool over = hovering(rect);
    if (over && active_ == kNoId) hotNext_ = control;

    bool changed = false;

    if (over && input_.mousePressed[static_cast<int>(MouseButton::Left)] && active_ == kNoId) {
        active_ = control;
        dragStartValue_ = normalised;
        dragStartPosition_ = input_.mousePosition;
    }

    if (active_ == control) {
        hotNext_ = control;
        // Vertical drag, because a rotary gesture on a small knob is fiddly and
        // every DAW has trained people to drag up and down.
        const float delta = -(input_.mousePosition.y - dragStartPosition_.y);
        const float scale = input_.shift ? kFineDragMultiplier : 1.0f;
        const float updated = clampValue(dragStartValue_ + (delta / kKnobDragRange) * scale, 0.0f, 1.0f);
        if (updated != normalised) { normalised = updated; changed = true; }
        setCursor(Cursor::ResizeVertical);
    } else if (over) {
        setCursor(Cursor::Hand);
    }

    if (over && input_.mouseDoubleClicked[static_cast<int>(MouseButton::Left)]) {
        normalised = clampValue(defaultValue, 0.0f, 1.0f);
        changed = true;
    }

    if (over && input_.wheel != 0.0f) {
        const float step = (input_.shift ? 0.005f : 0.02f) * (input_.wheel > 0.0f ? 1.0f : -1.0f);
        normalised = clampValue(normalised + step, 0.0f, 1.0f);
        changed = true;
    }

    const float endAngle = kKnobStartAngle + kKnobSweep * clampValue(normalised, 0.0f, 1.0f);
    const float trackThickness = std::max(2.0f, radius * 0.22f);

    // Unlit arc, then the value arc over it.
    list.addArc(centre, radius - trackThickness * 0.5f, kKnobStartAngle,
                kKnobStartAngle + kKnobSweep, t.widgetTrack, trackThickness);
    list.addArc(centre, radius - trackThickness * 0.5f, kKnobStartAngle, endAngle,
                fill, trackThickness);

    // Body and pointer.
    list.addCircleFilled(centre, radius - trackThickness * 1.35f, t.widgetBackground);
    list.addCircle(centre, radius - trackThickness * 1.35f,
                   (over || active_ == control) ? t.borderStrong : t.border, 1.0f);

    const Vec2 direction{ std::cos(endAngle), std::sin(endAngle) };
    list.addLine(centre + direction * (radius * 0.22f),
                 centre + direction * (radius - trackThickness * 1.6f),
                 (over || active_ == control) ? fill.brightened(1.3f) : fill, 2.0f);

    if (active_ == control)
        list.addCircleFilled(centre, radius + 3.0f, fill.withAlpha(0.10f));

    return changed;
}

// ---------------------------------------------------------------------------
// Parameter-aware widgets
// ---------------------------------------------------------------------------

bool Ui::parameterKnob(const Rect& rect, Parameter& parameter, const Colour& fill, bool showLabel) {
    const Theme& t = theme();
    const UiId control = idFrom(&parameter);

    Rect knobArea = rect;
    Rect labelArea;
    Rect valueArea;

    if (showLabel && rect.height > 46.0f) {
        labelArea = knobArea.removeFromTop(13.0f);
        valueArea = knobArea.removeFromBottom(13.0f);
    }

    float normalised = parameter.normalised();
    const bool changed = knob(control, knobArea, normalised, fill,
                              parameter.toNormalised(parameter.defaultValue()));
    if (changed) parameter.setNormalised(normalised);

    if (!labelArea.empty())
        drawList_.addTextClipped(font(t.fontSmall), labelArea, t.textDim, parameter.name(),
                                 DrawList::Align::Centre);

    if (!valueArea.empty()) {
        const bool interacting = isActive(control) || isHot(control);
        drawList_.addTextClipped(font(t.fontMono), valueArea,
                                 interacting ? t.text : t.textDim,
                                 parameter.toText(), DrawList::Align::Centre);
    }

    if (isHot(control) && !parameter.description().empty())
        setTooltip(parameter.name() + " - " + parameter.description());

    return changed;
}

bool Ui::parameterSlider(const Rect& rect, Parameter& parameter, const Colour& fill, bool showLabel) {
    const Theme& t = theme();
    const UiId control = idFrom(&parameter);

    Rect area = rect;
    Rect labelArea;
    if (showLabel && rect.width > 110.0f)
        labelArea = area.removeFromLeft(rect.width * 0.38f);

    Rect valueArea = area.removeFromRight(std::min(64.0f, area.width * 0.4f));
    area.removeFromRight(t.smallPadding);

    float normalised = parameter.normalised();
    const bool changed = sliderNormalised(control, area, normalised, fill);
    if (changed) parameter.setNormalised(normalised);

    if (!labelArea.empty())
        drawList_.addTextClipped(font(t.fontSmall), labelArea, t.textDim, parameter.name());

    drawList_.addTextClipped(font(t.fontMono), valueArea,
                             isHot(control) || isActive(control) ? t.text : t.textDim,
                             parameter.toText(), DrawList::Align::Right);

    if (isHot(control) && !parameter.description().empty())
        setTooltip(parameter.name() + " - " + parameter.description());

    return changed;
}

bool Ui::parameterToggle(const Rect& rect, Parameter& parameter, const Colour& fill) {
    const UiId control = idFrom(&parameter);
    const bool on = parameter.boolValue();

    if (button(control, rect, parameter.name(), ButtonStyle::Toggle, on)) {
        parameter.setValue(on ? 0.0f : 1.0f);
        return true;
    }

    if (isHot(control) && !parameter.description().empty())
        setTooltip(parameter.name() + " - " + parameter.description());

    (void)fill;
    return false;
}

bool Ui::parameterChoice(const Rect& rect, Parameter& parameter) {
    const UiId control = idFrom(&parameter);
    int selected = parameter.intValue();

    if (combo(control, rect, parameter.choices(), selected)) {
        parameter.setValue(static_cast<float>(selected));
        return true;
    }
    return false;
}

bool Ui::parameterRow(const Rect& rect, Parameter& parameter, const Colour& fill) {
    const Theme& t = theme();

    switch (parameter.kind()) {
        case ParamKind::Bool: {
            Rect area = rect;
            const Rect nameArea = area.removeFromLeft(rect.width * 0.55f);
            drawList_.addTextClipped(font(t.fontUi), nameArea, t.textDim, parameter.name());

            bool value = parameter.boolValue();
            if (checkbox(idFrom(&parameter), area, {}, value)) {
                parameter.setValue(value ? 1.0f : 0.0f);
                return true;
            }
            return false;
        }

        case ParamKind::Choice: {
            Rect area = rect;
            const Rect nameArea = area.removeFromLeft(rect.width * 0.42f);
            drawList_.addTextClipped(font(t.fontUi), nameArea, t.textDim, parameter.name());
            return parameterChoice(area.deflated(1.0f), parameter);
        }

        default:
            return parameterSlider(rect, parameter, fill);
    }
}

// ---------------------------------------------------------------------------
// Text editing
// ---------------------------------------------------------------------------

void Ui::beginTextEdit(UiId control, const std::string& initial, bool selectAll) {
    editing_ = control;
    textEdit_.buffer = initial;
    textEdit_.cursor = initial.size();
    textEdit_.selectionAnchor = selectAll ? 0 : initial.size();
    textEdit_.scrollX = 0.0f;
}

void Ui::cancelTextEdit() {
    editing_ = kNoId;
    textEdit_ = TextEditState{};
}

void Ui::deleteSelection() {
    if (!textEdit_.hasSelection()) return;

    const std::size_t from = std::min(textEdit_.cursor, textEdit_.selectionAnchor);
    const std::size_t to = std::max(textEdit_.cursor, textEdit_.selectionAnchor);
    textEdit_.buffer.erase(from, to - from);
    textEdit_.cursor = from;
    textEdit_.selectionAnchor = from;
}

void Ui::insertText(std::string_view utf8) {
    deleteSelection();
    textEdit_.buffer.insert(textEdit_.cursor, utf8);
    textEdit_.cursor += utf8.size();
    textEdit_.selectionAnchor = textEdit_.cursor;
}

bool Ui::textField(UiId control, const Rect& rect, std::string& text,
                   std::string_view placeholder, bool selectAllOnFocus) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;
    const gfx::Font& f = font(t.fontUi);

    const bool over = hovering(rect);
    const bool editing = editing_ == control;

    if (over) {
        setCursor(Cursor::Text);
        if (active_ == kNoId) hotNext_ = control;
    }

    if (over && input_.mousePressed[static_cast<int>(MouseButton::Left)]) {
        if (!editing) beginTextEdit(control, text, selectAllOnFocus);
        // Place the caret where the click landed.
        const float localX = input_.mousePosition.x - (rect.left() + t.smallPadding) + textEdit_.scrollX;
        textEdit_.cursor = f.offsetForX(textEdit_.buffer, localX);
        if (!input_.shift) textEdit_.selectionAnchor = textEdit_.cursor;
        active_ = control;
    } else if (!over && input_.mousePressed[static_cast<int>(MouseButton::Left)] && editing) {
        // Clicking away commits, which is what people expect from a field that
        // has no OK button.
        text = textEdit_.buffer;
        cancelTextEdit();
        return true;
    }

    if (editing && active_ == control && input_.mouseDown[static_cast<int>(MouseButton::Left)]) {
        const float localX = input_.mousePosition.x - (rect.left() + t.smallPadding) + textEdit_.scrollX;
        textEdit_.cursor = f.offsetForX(textEdit_.buffer, localX);
    }

    bool committed = false;

    if (editing) {
        updateTextEdit(rect, f);

        if (input_.keyPressed(key::Enter)) {
            text = textEdit_.buffer;
            cancelTextEdit();
            committed = true;
        }
    }

    // -- drawing -----------------------------------------------------------
    list.addRectFilled(rect, editing ? t.panelSunken : t.widgetBackground, t.cornerRadius);
    list.addRect(rect, editing ? t.accent.withAlpha(0.8f) : (over ? t.borderStrong : t.border),
                 t.borderWidth, t.cornerRadius);

    Rect inner = rect.deflated(t.smallPadding);
    list.pushClip(inner);

    const std::string& displayed = editing ? textEdit_.buffer : text;
    const float textY = inner.top() + (inner.height - f.lineHeight) * 0.5f;

    if (displayed.empty() && !placeholder.empty() && !editing) {
        list.addText(f, { inner.left(), textY }, t.textFaint, placeholder);
    } else {
        const float x = inner.left() - (editing ? textEdit_.scrollX : 0.0f);

        if (editing && textEdit_.hasSelection()) {
            const std::size_t from = std::min(textEdit_.cursor, textEdit_.selectionAnchor);
            const std::size_t to = std::max(textEdit_.cursor, textEdit_.selectionAnchor);
            const float x0 = x + f.textWidth(std::string_view(displayed).substr(0, from));
            const float x1 = x + f.textWidth(std::string_view(displayed).substr(0, to));
            list.addRectFilled(Rect{ x0, inner.top(), x1 - x0, inner.height },
                               t.accent.withAlpha(0.25f));
        }

        list.addText(f, { x, textY }, t.text, displayed);

        if (editing) {
            // A caret that blinks but never disappears for long enough to lose.
            const float caretX = x + f.textWidth(std::string_view(displayed).substr(0, textEdit_.cursor));
            static float blink = 0.0f;
            blink += deltaSeconds_;
            if (std::fmod(blink, 1.06f) < 0.62f)
                list.addRectFilled(Rect{ caretX, inner.top() + 1.0f, 1.5f, inner.height - 2.0f },
                                   t.accent);
        }
    }

    list.popClip();
    return committed;
}

void Ui::updateTextEdit(const Rect& rect, const gfx::Font& f) {
    const Theme& t = theme();

    for (std::uint32_t codepoint : input_.textInput) {
        if (codepoint < 0x20 || codepoint == 0x7F) continue;
        std::string encoded;
        appendUtf8(encoded, codepoint);
        insertText(encoded);
    }

    for (const InputState::KeyEvent& event : input_.keyEvents) {
        if (!event.pressed) continue;

        switch (event.code) {
            case key::Backspace:
                if (textEdit_.hasSelection()) { deleteSelection(); break; }
                if (textEdit_.cursor > 0) {
                    const std::size_t previous = utf8PrevOffset(textEdit_.buffer, textEdit_.cursor);
                    textEdit_.buffer.erase(previous, textEdit_.cursor - previous);
                    textEdit_.cursor = previous;
                    textEdit_.clearSelection();
                }
                break;

            case key::Delete:
                if (textEdit_.hasSelection()) { deleteSelection(); break; }
                if (textEdit_.cursor < textEdit_.buffer.size()) {
                    const std::size_t next = utf8NextOffset(textEdit_.buffer, textEdit_.cursor);
                    textEdit_.buffer.erase(textEdit_.cursor, next - textEdit_.cursor);
                }
                break;

            case key::Left:
                textEdit_.cursor = utf8PrevOffset(textEdit_.buffer, textEdit_.cursor);
                if (!input_.shift) textEdit_.clearSelection();
                break;

            case key::Right:
                textEdit_.cursor = utf8NextOffset(textEdit_.buffer, textEdit_.cursor);
                if (!input_.shift) textEdit_.clearSelection();
                break;

            case key::Home:
                textEdit_.cursor = 0;
                if (!input_.shift) textEdit_.clearSelection();
                break;

            case key::End:
                textEdit_.cursor = textEdit_.buffer.size();
                if (!input_.shift) textEdit_.clearSelection();
                break;

            case key::A:
                if (input_.ctrl) {
                    textEdit_.selectionAnchor = 0;
                    textEdit_.cursor = textEdit_.buffer.size();
                }
                break;

            default:
                break;
        }
    }

    // Keep the caret in view.
    const Rect inner = rect.deflated(t.smallPadding);
    const float caretX = f.textWidth(std::string_view(textEdit_.buffer).substr(0, textEdit_.cursor));
    if (caretX - textEdit_.scrollX > inner.width - 4.0f)
        textEdit_.scrollX = caretX - inner.width + 4.0f;
    if (caretX - textEdit_.scrollX < 0.0f)
        textEdit_.scrollX = caretX;
    textEdit_.scrollX = std::max(0.0f, textEdit_.scrollX);
}

// ---------------------------------------------------------------------------
// Popups
// ---------------------------------------------------------------------------

void Ui::openPopup(UiId control, Vec2 anchor, Vec2 size) {
    popupId_ = control;
    popupJustOpened_ = true;

    // Flip rather than clip when the popup would fall off an edge.
    float x = anchor.x;
    float y = anchor.y;
    if (x + size.x > displaySize_.x - 4.0f) x = std::max(4.0f, displaySize_.x - size.x - 4.0f);
    if (y + size.y > displaySize_.y - 4.0f) y = std::max(4.0f, anchor.y - size.y);

    popupRect_ = Rect{ x, y, size.x, size.y };
}

bool Ui::beginPopup(UiId control, Rect& outRect) {
    if (popupId_ != control) return false;
    outRect = popupRect_;
    insidePopup_ = true;

    const Theme& t = theme();
    overlayList_.addRectFilled(popupRect_.translated({ 0.0f, 3.0f }),
                               Colour{ 0.0f, 0.0f, 0.0f, 0.45f }, t.cornerRadiusLarge);
    overlayList_.addRectFilled(popupRect_, t.panelRaised, t.cornerRadiusLarge);
    overlayList_.addRect(popupRect_, t.borderStrong, t.borderWidth, t.cornerRadiusLarge);
    overlayList_.pushClip(popupRect_);
    return true;
}

void Ui::endPopup() {
    if (!insidePopup_) return;
    overlayList_.popClip();
    insidePopup_ = false;
}

void Ui::closePopup() {
    popupId_ = kNoId;
    popupRect_ = Rect{};
}

int Ui::popupMenu(UiId control, const Rect& rect, const std::vector<std::string>& items,
                  int selected, const std::vector<bool>* enabled) {
    const Theme& t = theme();
    int chosen = -1;

    Rect cursorRect = rect.deflated(t.smallPadding);

    // A list too long for the popup scrolls rather than being cut off. It used
    // to simply stop drawing when it ran out of room, so a 24-output interface
    // silently offered 14 channels and there was no way to reach the rest.
    const float contentHeight = static_cast<float>(items.size()) * t.rowHeight;
    const float maximumOffset = std::max(0.0f, contentHeight - cursorRect.height);

    ScrollState* scroll = nullptr;
    if (maximumOffset > 0.0f) {
        for (ScrollState& candidate : scrollStates_)
            if (candidate.id == control) { scroll = &candidate; break; }
        if (!scroll) {
            scrollStates_.push_back(ScrollState{ control, 0.0f, contentHeight, rect });
            scroll = &scrollStates_.back();
        }

        // The popup owns the pointer while it is open, so the wheel needs no
        // hover test beyond being inside it.
        if (rect.contains(input_.mousePosition) && input_.wheel != 0.0f)
            scroll->offset -= input_.wheel * t.rowHeight * 3.0f;
        scroll->offset = clampValue(scroll->offset, 0.0f, maximumOffset);

        // Keep the current choice on screen when the popup is first opened,
        // otherwise picking output 20 means scrolling to find where you are.
        if (popupJustOpened_ && selected >= 0) {
            const float selectedTop = static_cast<float>(selected) * t.rowHeight;
            if (selectedTop < scroll->offset)
                scroll->offset = selectedTop;
            else if (selectedTop + t.rowHeight > scroll->offset + cursorRect.height)
                scroll->offset = selectedTop + t.rowHeight - cursorRect.height;
            scroll->offset = clampValue(scroll->offset, 0.0f, maximumOffset);
        }

        cursorRect.width -= t.scrollBarWidth;
        cursorRect.y -= scroll->offset;
        cursorRect.height = contentHeight;
    }

    const Rect visible = rect.deflated(t.smallPadding);

    for (std::size_t i = 0; i < items.size(); ++i) {
        const Rect row = cursorRect.removeFromTop(t.rowHeight);
        // Rows scrolled out of view are skipped, not stopped at.
        if (row.bottom() < visible.top() || row.top() > visible.bottom()) continue;

        const bool itemEnabled = !enabled || i >= enabled->size() || (*enabled)[i];
        // Salted from the popup's own id. Keying these off the address of the
        // item vector meant two combos whose lists happened to live at the same
        // stack address shared every row id between them.
        const UiId itemId = control ^ (static_cast<UiId>(i + 1) * 0x9E3779B97F4A7C15ull);

        bool hovered = false, held = false;
        const bool clicked = itemEnabled && buttonBehaviour(itemId, row, hovered, held);

        if (hovered && itemEnabled)
            overlayList_.addRectFilled(row, t.widgetHover, t.cornerRadius);

        const bool isSelected = static_cast<int>(i) == selected;
        const Colour textColour = !itemEnabled ? t.textFaint
                                : isSelected ? t.accent
                                : hovered ? t.text : t.textDim;

        Rect textRect = row;
        textRect.removeFromLeft(t.padding);
        overlayList_.addTextClipped(font(t.fontUi), textRect, textColour, items[i]);

        if (isSelected) {
            overlayList_.addRectFilled(Rect{ row.left() + 3.0f, row.centre().y - 2.0f, 3.0f, 4.0f },
                                       t.accent, 1.5f);
        }

        if (clicked) chosen = static_cast<int>(i);
    }

    // A slim bar, so it is obvious there is more list below.
    if (maximumOffset > 0.0f && scroll != nullptr) {
        const Rect track{ visible.right() - t.scrollBarWidth + 2.0f, visible.top(),
                          t.scrollBarWidth - 2.0f, visible.height };
        const float thumbHeight = std::max(24.0f, track.height * (visible.height / contentHeight));
        const float progress = scroll->offset / maximumOffset;
        overlayList_.addRectFilled(track, t.widgetTrack, 2.0f);
        overlayList_.addRectFilled(Rect{ track.left(), track.top()
                                             + (track.height - thumbHeight) * progress,
                                         track.width, thumbHeight },
                                   t.borderStrong, 2.0f);
    }

    return chosen;
}

bool Ui::combo(UiId control, const Rect& rect, const std::vector<std::string>& items,
               int& selectedIndex) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    bool hovered = false, held = false;
    const bool clicked = buttonBehaviour(control, rect, hovered, held);

    list.addRectFilled(rect, hovered ? t.widgetHover : t.widgetBackground, t.cornerRadius);
    list.addRect(rect, popupOpen(control) ? t.accent.withAlpha(0.8f) : (hovered ? t.borderStrong : t.border),
                 t.borderWidth, t.cornerRadius);

    Rect textRect = rect.deflated(t.smallPadding);
    const Rect arrowRect = textRect.removeFromRight(12.0f);

    const std::string_view current =
        (selectedIndex >= 0 && selectedIndex < static_cast<int>(items.size()))
            ? std::string_view(items[static_cast<std::size_t>(selectedIndex)])
            : std::string_view("-");
    list.addTextClipped(font(t.fontUi), textRect, hovered ? t.text : t.textDim, current);
    drawIcon(list, arrowRect, Icon::Chevron, hovered ? t.text : t.textFaint);

    if (clicked) {
        if (popupOpen(control)) {
            closePopup();
        } else {
            // Sized to what the list wants, capped by the room actually below
            // the control - a fixed ceiling meant a long list was cut off on a
            // tall display that had plenty of space for it.
            const float wanted = static_cast<float>(items.size()) * t.rowHeight
                               + t.smallPadding * 2.0f;
            const float below = displaySize_.y - rect.bottom() - 10.0f;
            const float above = rect.top() - 10.0f;
            const float height = std::min(wanted, std::max(std::max(below, above), 120.0f));
            openPopup(control, { rect.left(), rect.bottom() + 2.0f }, { rect.width, height });
        }
    }

    bool changed = false;
    Rect popupRect;
    if (beginPopup(control, popupRect)) {
        const int chosen = popupMenu(control, popupRect, items, selectedIndex);
        if (chosen >= 0) {
            selectedIndex = chosen;
            changed = true;
            closePopup();
        }
        endPopup();
    }

    if (hovered) setCursor(Cursor::Hand);
    return changed;
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

float Ui::scrollOffset(UiId control) const {
    for (const ScrollState& state : scrollStates_)
        if (state.id == control) return state.offset;
    return 0.0f;
}

void Ui::setScrollOffset(UiId control, float offset) {
    for (ScrollState& state : scrollStates_)
        if (state.id == control) { state.offset = offset; return; }
    scrollStates_.push_back(ScrollState{ control, offset, 0.0f, Rect{} });
}

bool Ui::intField(UiId control, const Rect& rect, int& value, int lo, int hi) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    bool hovered = false, held = false;
    buttonBehaviour(control, rect, hovered, held);

    bool changed = false;
    if (isActive(control)) {
        // Two pixels per step: fine enough to land on a bar exactly, coarse
        // enough to cross a whole song without letting go.
        dragAccumulator_ += input_.mouseDelta.x * (input_.shift ? 0.1f : 0.5f);
        const int steps = static_cast<int>(dragAccumulator_);
        if (steps != 0) {
            dragAccumulator_ -= static_cast<float>(steps);
            const int next = clampValue(value + steps, lo, hi);
            if (next != value) { value = next; changed = true; }
        }
    } else {
        dragAccumulator_ = 0.0f;
    }

    list.addRectFilled(rect, hovered ? t.widgetHover : t.widgetBackground, t.cornerRadius);
    list.addRect(rect, t.border, t.borderWidth, t.cornerRadius);
    list.addTextClipped(font(t.fontSmall), rect.deflated(t.smallPadding),
                        hovered ? t.text : t.textDim, std::to_string(value),
                        DrawList::Align::Centre);

    if (hovered) setCursor(Cursor::ResizeHorizontal);
    return changed;
}

Rect Ui::beginScroll(UiId control, const Rect& rect, float contentHeight) {
    ScrollState* state = nullptr;
    for (ScrollState& candidate : scrollStates_)
        if (candidate.id == control) { state = &candidate; break; }

    if (!state) {
        scrollStates_.push_back(ScrollState{ control, 0.0f, contentHeight, rect });
        state = &scrollStates_.back();
    }

    state->contentHeight = contentHeight;
    state->viewRect = rect;

    const float maximum = std::max(0.0f, contentHeight - rect.height);
    if (hovering(rect) && input_.wheel != 0.0f && active_ == kNoId)
        state->offset -= input_.wheel * 48.0f;
    state->offset = clampValue(state->offset, 0.0f, maximum);

    scrollStack_.push_back(control);
    drawList_.pushClip(rect);

    // Content is laid out in the returned rect, which is the view shifted up by
    // the scroll offset; nothing inside needs to know it is in a scroller.
    Rect content = rect;
    content.y -= state->offset;
    content.height = std::max(contentHeight, rect.height);
    if (maximum > 0.0f) content.width -= theme().scrollBarWidth;
    return content;
}

void Ui::endScroll() {
    if (scrollStack_.empty()) return;

    const UiId control = scrollStack_.back();
    scrollStack_.pop_back();
    drawList_.popClip();

    ScrollState* state = nullptr;
    for (ScrollState& candidate : scrollStates_)
        if (candidate.id == control) { state = &candidate; break; }
    if (!state) return;

    const float maximum = state->contentHeight - state->viewRect.height;
    if (maximum <= 0.0f) return;   // nothing to scroll, so no bar

    const Theme& t = theme();
    const Rect view = state->viewRect;
    const Rect track{ view.right() - t.scrollBarWidth, view.top(), t.scrollBarWidth, view.height };

    const float thumbHeight = std::max(28.0f, view.height * (view.height / state->contentHeight));
    const float travel = track.height - thumbHeight;
    const float progress = maximum > 0.0f ? state->offset / maximum : 0.0f;
    const Rect thumb{ track.left() + 2.0f, track.top() + travel * progress,
                      track.width - 4.0f, thumbHeight };

    const UiId thumbId = control ^ 0x5CBA11ull;
    bool hovered = false, held = false;
    buttonBehaviour(thumbId, thumb, hovered, held);

    if (isActive(thumbId)) {
        // Drag maps thumb travel back onto content travel.
        const float delta = input_.mouseDelta.y;
        if (travel > 0.0f) state->offset = clampValue(state->offset + delta * (maximum / travel),
                                                      0.0f, maximum);
    } else if (hovering(track) && input_.mousePressed[static_cast<int>(MouseButton::Left)]
               && !thumb.contains(input_.mousePosition)) {
        // Clicking the track pages towards the click.
        state->offset = clampValue(state->offset
                                       + (input_.mousePosition.y < thumb.top() ? -view.height : view.height),
                                   0.0f, maximum);
    }

    drawList_.addRectFilled(track, t.panelSunken.withAlpha(0.6f), t.cornerRadius);
    drawList_.addRectFilled(thumb, (hovered || isActive(thumbId)) ? t.borderStrong : t.border,
                            t.cornerRadius);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------

void Ui::meter(const Rect& rect, float level, bool vertical) {
    const Theme& t = theme();
    DrawList& list = insidePopup_ ? overlayList_ : drawList_;

    list.addRectFilled(rect, t.meterBackground, 1.5f);

    level = clampValue(level, 0.0f, 1.4f);
    if (level <= 0.0005f) return;

    // Meters are drawn on a decibel scale: a linear bar spends most of its
    // length in a range nobody performs in.
    const float db = 20.0f * std::log10(std::max(level, 1.0e-4f));
    const float normalised = clampValue((db + 60.0f) / 60.0f, 0.0f, 1.0f);

    // Segmented, because a continuous bar is hard to read at a glance.
    constexpr int kSegments = 24;
    const int lit = static_cast<int>(normalised * kSegments + 0.5f);

    for (int i = 0; i < lit; ++i) {
        const float t01 = static_cast<float>(i) / static_cast<float>(kSegments - 1);
        const Colour colour = t.meterColour(t01);

        Rect segment;
        if (vertical) {
            const float segmentHeight = rect.height / kSegments;
            segment = Rect{ rect.left(), rect.bottom() - segmentHeight * (i + 1),
                            rect.width, segmentHeight - 1.0f };
        } else {
            const float segmentWidth = rect.width / kSegments;
            segment = Rect{ rect.left() + segmentWidth * i, rect.top(),
                            segmentWidth - 1.0f, rect.height };
        }
        if (segment.width > 0.0f && segment.height > 0.0f)
            list.addRectFilled(segment, colour, 0.0f);
    }

    // Over full scale gets a hard marker, latched for the frame.
    if (level >= 0.999f) {
        const Rect marker = vertical ? Rect{ rect.left(), rect.top(), rect.width, 2.0f }
                                     : Rect{ rect.right() - 2.0f, rect.top(), 2.0f, rect.height };
        list.addRectFilled(marker, t.danger);
    }
}

void Ui::stereoMeter(const Rect& rect, float left, float right, bool vertical) {
    const float gap = 2.0f;
    if (vertical) {
        const float width = (rect.width - gap) * 0.5f;
        meter(Rect{ rect.left(), rect.top(), width, rect.height }, left, true);
        meter(Rect{ rect.left() + width + gap, rect.top(), width, rect.height }, right, true);
    } else {
        const float height = (rect.height - gap) * 0.5f;
        meter(Rect{ rect.left(), rect.top(), rect.width, height }, left, false);
        meter(Rect{ rect.left(), rect.top() + height + gap, rect.width, height }, right, false);
    }
}

// ---------------------------------------------------------------------------
// Tooltips, notifications, drag
// ---------------------------------------------------------------------------

void Ui::setTooltip(std::string text) {
    tooltip_ = std::move(text);
    tooltipPosition_ = input_.mousePosition;
}

void Ui::drawTooltip() {
    if (tooltip_.empty()) return;

    const Theme& t = theme();
    const gfx::Font& f = font(t.fontSmall);
    const float width = f.textWidth(tooltip_) + t.padding * 2.0f;
    const float height = f.lineHeight + t.smallPadding * 2.0f;

    float x = tooltipPosition_.x + 16.0f;
    float y = tooltipPosition_.y + 20.0f;
    if (x + width > displaySize_.x - 4.0f) x = displaySize_.x - width - 4.0f;
    if (y + height > displaySize_.y - 4.0f) y = tooltipPosition_.y - height - 8.0f;

    const Rect rect{ x, y, width, height };
    overlayList_.addRectFilled(rect.translated({ 0.0f, 2.0f }),
                               Colour{ 0.0f, 0.0f, 0.0f, 0.5f }, t.cornerRadius);
    overlayList_.addRectFilled(rect, t.panelRaised, t.cornerRadius);
    overlayList_.addRect(rect, t.borderStrong, t.borderWidth, t.cornerRadius);
    overlayList_.addTextClipped(f, rect.deflated(t.padding * 0.5f), t.text, tooltip_,
                                DrawList::Align::Centre);
}

void Ui::notify(std::string text, const Colour& colour, float seconds) {
    notification_ = std::move(text);
    notificationColour_ = colour;
    notificationTimer_ = seconds;
}

void Ui::drawNotification() {
    if (notificationTimer_ <= 0.0f || notification_.empty()) return;

    const Theme& t = theme();
    const gfx::Font& f = font(t.fontUi);
    const float width = std::min(f.textWidth(notification_) + t.padding * 3.0f,
                                 displaySize_.x - 40.0f);
    const float height = f.lineHeight + t.padding * 1.5f;

    // Fade out over the last half second rather than vanishing.
    const float alpha = clampValue(notificationTimer_ / 0.5f, 0.0f, 1.0f);

    const Rect rect{ (displaySize_.x - width) * 0.5f, displaySize_.y - height - 48.0f,
                     width, height };

    overlayList_.pushAlpha(alpha);
    overlayList_.addRectFilled(rect.translated({ 0.0f, 3.0f }),
                               Colour{ 0.0f, 0.0f, 0.0f, 0.5f }, t.cornerRadiusLarge);
    overlayList_.addRectFilled(rect, t.panelRaised, t.cornerRadiusLarge);
    overlayList_.addRect(rect, notificationColour_.withAlpha(0.7f), t.borderWidth, t.cornerRadiusLarge);
    overlayList_.addRectFilled(Rect{ rect.left(), rect.top(), 3.0f, rect.height },
                               notificationColour_, t.cornerRadiusLarge);
    overlayList_.addTextClipped(f, rect.deflated(t.padding), t.text, notification_,
                                DrawList::Align::Centre);
    overlayList_.popAlpha();
}

void Ui::beginDrag(std::string type, std::string payload) {
    dragType_ = std::move(type);
    dragPayload_ = std::move(payload);
}

bool Ui::acceptDrop(const Rect& rect, std::string_view type) {
    if (!dragReleasedThisFrame_ || dragType_ != type) return false;
    if (!rect.contains(input_.mousePosition)) return false;

    // The payload is deliberately left intact: the caller reads it *after* this
    // returns, and clearing it here handed every drop target an empty string.
    // endFrame clears it, once the frame is genuinely over.
    //
    // Clearing the released flag is what keeps a single drop from being taken
    // twice by two overlapping targets.
    dragReleasedThisFrame_ = false;
    return true;
}

void Ui::cancelDrag() {
    dragType_.clear();
    dragPayload_.clear();
}

} // namespace acm::ui

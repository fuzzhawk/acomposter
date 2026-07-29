// The dark lab palette.
//
// The look: near-black with a cold cast, thin hairline borders, and colour used
// sparingly and only where it carries meaning. Signal is phosphor teal, controls
// are amber, faults are coral. Everything else is grey. In a dim room with a
// projector behind you, anything brighter than this is a liability.
//
// Node categories each own an accent, which the canvas uses for the node's
// header, its port rings and the cables leaving it - so a patch is readable as a
// shape and a colour before any label is read.
#pragma once

#include "../core/Node.h"
#include "../gfx/FontAtlas.h"
#include "../gfx/Geometry.h"

namespace acm::ui {

using gfx::Colour;

struct Theme {
    // -- surfaces ----------------------------------------------------------
    Colour background      = Colour::fromHex(0x08090B);   // the void behind everything
    Colour canvas          = Colour::fromHex(0x0B0D10);
    Colour canvasGrid      = Colour::fromHex(0x131820);
    Colour canvasGridMajor = Colour::fromHex(0x1A222C);

    Colour panel           = Colour::fromHex(0x101317);
    Colour panelRaised     = Colour::fromHex(0x161A20);
    Colour panelSunken     = Colour::fromHex(0x0A0C0F);
    Colour panelHeader     = Colour::fromHex(0x14181E);

    Colour border          = Colour::fromHex(0x232A33);
    Colour borderStrong    = Colour::fromHex(0x323C48);
    Colour borderFaint     = Colour::fromHex(0x181D24);

    // -- text --------------------------------------------------------------
    Colour text            = Colour::fromHex(0xC6D0DA);
    Colour textDim         = Colour::fromHex(0x7C8894);
    Colour textFaint       = Colour::fromHex(0x4C5560);
    Colour textOnAccent    = Colour::fromHex(0x06090B);

    // -- accents -----------------------------------------------------------
    Colour accent          = Colour::fromHex(0x3FE0C0);   // phosphor teal: signal
    Colour accentDim       = Colour::fromHex(0x1E7A6C);
    Colour control         = Colour::fromHex(0xE9A13B);   // amber: things you grab
    Colour warning         = Colour::fromHex(0xE9C13B);
    Colour danger          = Colour::fromHex(0xFF5F5F);
    Colour success         = Colour::fromHex(0x5FD98A);
    Colour recording       = Colour::fromHex(0xFF4A5C);

    // -- widgets -----------------------------------------------------------
    Colour widgetBackground = Colour::fromHex(0x0D1015);
    Colour widgetHover      = Colour::fromHex(0x1B212A);
    Colour widgetActive     = Colour::fromHex(0x232C37);
    Colour widgetTrack      = Colour::fromHex(0x191F27);
    Colour selection        = Colour::fromHex(0x3FE0C0);

    // -- metering ----------------------------------------------------------
    Colour meterLow        = Colour::fromHex(0x3FE0C0);
    Colour meterMid        = Colour::fromHex(0xE9C13B);
    Colour meterHigh       = Colour::fromHex(0xFF5F5F);
    Colour meterBackground = Colour::fromHex(0x0A0D11);

    // -- cables ------------------------------------------------------------
    Colour cable           = Colour::fromHex(0x4A5866);
    Colour cableHover      = Colour::fromHex(0x8FA0B2);
    Colour cableFeedback   = Colour::fromHex(0xE9A13B);   // one-block delay edges
    float cableThickness   = 2.0f;

    // -- metrics -----------------------------------------------------------
    float cornerRadius       = 3.0f;
    float cornerRadiusLarge  = 5.0f;
    float borderWidth        = 1.0f;
    float rowHeight          = 22.0f;
    float smallRowHeight     = 18.0f;
    float padding            = 8.0f;
    float smallPadding       = 4.0f;
    float portRadius         = 5.0f;
    float nodeHeaderHeight   = 24.0f;
    float nodeMinWidth       = 168.0f;
    float scrollBarWidth     = 10.0f;

    // Display scale the fonts were rasterised at. Every structural dimension -
    // bar heights, panel widths, row heights - has to be multiplied by this, or
    // the text grows on a high-DPI display while its container does not and the
    // labels clip. Use scaled() rather than reaching for raw pixel constants.
    float scale = 1.0f;
    float scaled(float pixels) const noexcept { return pixels * scale; }

    // -- fonts (indices into the atlas, filled in at start-up) -------------
    gfx::FontId fontUi = gfx::kInvalidFont;
    gfx::FontId fontUiBold = gfx::kInvalidFont;
    gfx::FontId fontSmall = gfx::kInvalidFont;
    gfx::FontId fontMono = gfx::kInvalidFont;
    gfx::FontId fontTitle = gfx::kInvalidFont;

    // The accent a node of this category wears.
    Colour categoryColour(NodeCategory category) const;

    // Green through amber to red, by level. Used by every meter in the app.
    Colour meterColour(float level) const;
};

// The single instance the whole UI reads from.
Theme& theme();

// Queues the application's fonts into `atlas` and records their ids in the
// theme. Call before FontAtlas::build().
void registerThemeFonts(gfx::FontAtlas& atlas, float uiScale);

} // namespace acm::ui

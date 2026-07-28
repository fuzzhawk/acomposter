#include "Theme.h"

namespace acm::ui {

Theme& theme() {
    static Theme instance;
    return instance;
}

Colour Theme::categoryColour(NodeCategory category) const {
    switch (category) {
        case NodeCategory::Source:   return Colour::fromHex(0x3FE0C0);   // teal
        case NodeCategory::Effect:   return Colour::fromHex(0xA98BF0);   // violet
        case NodeCategory::Mixing:   return Colour::fromHex(0xE9A13B);   // amber
        case NodeCategory::Routing:  return Colour::fromHex(0x6BB6F0);   // sky
        case NodeCategory::Output:   return Colour::fromHex(0xFF8A6B);   // coral
        case NodeCategory::Analysis: return Colour::fromHex(0x8FE07A);   // mint
    }
    return accent;
}

Colour Theme::meterColour(float level) const {
    // Thresholds in linear amplitude: -12 dBFS and -3 dBFS, where a performer
    // starts caring and where they start worrying.
    constexpr float midThreshold = 0.25f;
    constexpr float highThreshold = 0.71f;

    if (level < midThreshold) return meterLow;
    if (level < highThreshold)
        return gfx::lerp(meterLow, meterMid, (level - midThreshold) / (highThreshold - midThreshold));
    return gfx::lerp(meterMid, meterHigh,
                     clampValue((level - highThreshold) / (1.0f - highThreshold), 0.0f, 1.0f));
}

void registerThemeFonts(gfx::FontAtlas& atlas, float uiScale) {
    Theme& t = theme();

    const float scale = clampValue(uiScale, 0.75f, 3.0f);

    // Segoe UI is on every Windows 10 install; Consolas is the numeric face,
    // because parameter values in a proportional font jitter as they change.
    t.fontUi = atlas.addFont("Segoe UI", 13.0f * scale, false, false);
    t.fontUiBold = atlas.addFont("Segoe UI", 13.0f * scale, true, false);
    t.fontSmall = atlas.addFont("Segoe UI", 11.0f * scale, false, false);
    t.fontMono = atlas.addFont("Consolas", 12.0f * scale, false, true);
    t.fontTitle = atlas.addFont("Segoe UI", 17.0f * scale, true, false);

    // Metrics scale with the fonts, so the whole interface tracks display DPI
    // rather than only the text.
    t.cornerRadius *= scale;
    t.cornerRadiusLarge *= scale;
    t.rowHeight *= scale;
    t.smallRowHeight *= scale;
    t.padding *= scale;
    t.smallPadding *= scale;
    t.portRadius *= scale;
    t.nodeHeaderHeight *= scale;
    t.nodeMinWidth *= scale;
    t.scrollBarWidth *= scale;
    t.cableThickness *= scale;
}

} // namespace acm::ui

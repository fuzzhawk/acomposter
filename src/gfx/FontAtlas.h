// Text rendering without a font library.
//
// acomposter ships no third-party code, so glyphs are rasterised with GDI at
// start-up and packed into a single texture. GetGlyphOutline with GGO_GRAY8
// hands back an 8-bit coverage bitmap per glyph, which is exactly what an
// alpha-blended atlas wants, and it means the application uses the same real
// Windows fonts as everything else on the machine rather than something baked in.
//
// The atlas also carries a small block of solid white texels. Every untextured
// shape in the draw list samples that block, so one texture and one shader cover
// an entire frame - no state changes between drawing a panel and its label.
#pragma once

#include "Geometry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace acm::gfx {

struct Glyph {
    // Position in the atlas, normalised.
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    // Offset from the pen position to the glyph's top-left corner, in pixels.
    float offsetX = 0.0f, offsetY = 0.0f;
    float width = 0.0f, height = 0.0f;
    float advance = 0.0f;
};

using FontId = int;
inline constexpr FontId kInvalidFont = -1;

struct Font {
    std::string family;
    float pixelHeight = 14.0f;
    float ascent = 0.0f;
    float descent = 0.0f;
    float lineHeight = 0.0f;
    bool bold = false;
    bool monospaced = false;
    // Set for fonts where every glyph shares an advance, so column layouts can
    // be computed without measuring.
    float fixedAdvance = 0.0f;

    // ASCII is the overwhelming majority of what gets drawn, so it gets a flat
    // array; anything above goes through the map.
    Glyph ascii[128];
    bool asciiPresent[128] = {};
    std::unordered_map<std::uint32_t, Glyph> extended;

    const Glyph* glyph(std::uint32_t codepoint) const;

    float textWidth(std::string_view utf8) const;
    // Byte offset of the character boundary nearest `x` pixels along the string.
    std::size_t offsetForX(std::string_view utf8, float x) const;
    // Longest prefix that fits in `maxWidth`, in bytes.
    std::size_t prefixFitting(std::string_view utf8, float maxWidth) const;
};

class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();

    // Queues a font. Nothing is rasterised until build() runs, so all the fonts
    // an application needs land in one texture.
    FontId addFont(const std::string& family, float pixelHeight,
                   bool bold = false, bool monospaced = false);

    // Rasterises everything queued. Returns false only if GDI itself fails.
    bool build();
    bool built() const noexcept { return built_; }

    const Font& font(FontId id) const;
    int fontCount() const noexcept { return static_cast<int>(fonts_.size()); }

    // RGBA8, width * height texels. White with coverage in the alpha channel.
    const std::uint32_t* pixels() const noexcept { return pixels_.data(); }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }

    Vec2 whitePixelUv() const noexcept { return whiteUv_; }

    // Frees the CPU-side pixels once they have been uploaded to the GPU.
    void releasePixels();

private:
    struct PendingFont {
        std::string family;
        float pixelHeight;
        bool bold;
        bool monospaced;
    };

    // Shelf packer: glyphs are added left to right on a row until it fills, then
    // a new row starts below the tallest glyph so far. Simple, and good enough
    // for a few hundred glyphs of similar height.
    bool packRect(int w, int h, int& outX, int& outY);
    void reserveWhiteBlock();

    std::vector<PendingFont> pending_;
    std::vector<Font> fonts_;

    std::vector<std::uint32_t> pixels_;
    int width_ = 0;
    int height_ = 0;

    int shelfX_ = 0;
    int shelfY_ = 0;
    int shelfHeight_ = 0;

    Vec2 whiteUv_{ 0.0f, 0.0f };
    bool built_ = false;
};

// The codepoints acomposter rasterises: printable ASCII, Latin-1, and the
// handful of symbols the interface uses for meters and transport labels.
struct CodepointRange {
    std::uint32_t first;
    std::uint32_t last;
};

const std::vector<CodepointRange>& defaultCodepointRanges();

} // namespace acm::gfx

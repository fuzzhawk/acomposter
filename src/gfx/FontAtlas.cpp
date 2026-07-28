#include "FontAtlas.h"

#include "../core/Utf.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

namespace acm::gfx {
namespace {

// The atlas starts here and doubles until everything fits. Two 14 px UI fonts
// plus a mono font comfortably fit 512x512.
constexpr int kInitialAtlasSize = 512;
constexpr int kMaxAtlasSize = 4096;
constexpr int kGlyphPadding = 1;   // stops bilinear sampling bleeding neighbours
constexpr int kWhiteBlockSize = 4;

// GGO_GRAY8_BITMAP quantises coverage to 0..64, not 0..255.
constexpr int kGrayLevels = 64;

// Decodes one UTF-8 code point, advancing `index`.
std::uint32_t nextCodepoint(std::string_view text, std::size_t& index) {
    if (index >= text.size()) return 0;

    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) { ++index; return lead; }

    int extra = 0;
    std::uint32_t codepoint = 0;
    if ((lead & 0xE0) == 0xC0)      { codepoint = lead & 0x1Fu; extra = 1; }
    else if ((lead & 0xF0) == 0xE0) { codepoint = lead & 0x0Fu; extra = 2; }
    else if ((lead & 0xF8) == 0xF0) { codepoint = lead & 0x07u; extra = 3; }
    else { ++index; return 0xFFFD; }

    if (index + static_cast<std::size_t>(extra) >= text.size()) { index = text.size(); return 0xFFFD; }

    for (int i = 1; i <= extra; ++i) {
        const auto continuation = static_cast<unsigned char>(text[index + static_cast<std::size_t>(i)]);
        if ((continuation & 0xC0) != 0x80) { ++index; return 0xFFFD; }
        codepoint = (codepoint << 6) | (continuation & 0x3Fu);
    }

    index += static_cast<std::size_t>(extra) + 1;
    return codepoint;
}

} // namespace

const std::vector<CodepointRange>& defaultCodepointRanges() {
    static const std::vector<CodepointRange> ranges = {
        { 0x0020, 0x007E },   // printable ASCII
        { 0x00A0, 0x00FF },   // Latin-1 supplement: degree, plus-minus, accents
        { 0x2018, 0x201D },   // curly quotes, which paste in from everywhere
        { 0x2022, 0x2022 },   // bullet
        { 0x2026, 0x2026 },   // ellipsis, used when truncating labels
        { 0x2190, 0x2193 },   // arrows
        { 0x25B2, 0x25BC },   // triangles, for transport and disclosure marks
        { 0x2660, 0x266F },   // musical symbols, for the tempo readout
    };
    return ranges;
}

// ---------------------------------------------------------------------------
// Font lookups
// ---------------------------------------------------------------------------

const Glyph* Font::glyph(std::uint32_t codepoint) const {
    if (codepoint < 128) {
        return asciiPresent[codepoint] ? &ascii[codepoint] : nullptr;
    }
    const auto it = extended.find(codepoint);
    return it != extended.end() ? &it->second : nullptr;
}

float Font::textWidth(std::string_view utf8) const {
    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::uint32_t codepoint = nextCodepoint(utf8, index);
        if (const Glyph* g = glyph(codepoint)) width += g->advance;
        else if (const Glyph* fallback = glyph(U'?')) width += fallback->advance;
    }
    return width;
}

std::size_t Font::offsetForX(std::string_view utf8, float x) const {
    if (x <= 0.0f) return 0;

    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::size_t start = index;
        const std::uint32_t codepoint = nextCodepoint(utf8, index);
        const Glyph* g = glyph(codepoint);
        const float advance = g ? g->advance : 0.0f;

        // Snap to whichever side of the glyph the point is nearer, so clicking
        // the right half of a character puts the caret after it.
        if (x < width + advance * 0.5f) return start;
        width += advance;
    }
    return utf8.size();
}

std::size_t Font::prefixFitting(std::string_view utf8, float maxWidth) const {
    float width = 0.0f;
    std::size_t index = 0;
    while (index < utf8.size()) {
        const std::size_t start = index;
        const std::uint32_t codepoint = nextCodepoint(utf8, index);
        const Glyph* g = glyph(codepoint);
        const float advance = g ? g->advance : 0.0f;
        if (width + advance > maxWidth) return start;
        width += advance;
    }
    return utf8.size();
}

// ---------------------------------------------------------------------------
// Atlas
// ---------------------------------------------------------------------------

FontAtlas::FontAtlas() = default;
FontAtlas::~FontAtlas() = default;

FontId FontAtlas::addFont(const std::string& family, float pixelHeight, bool bold, bool monospaced) {
    pending_.push_back(PendingFont{ family, pixelHeight, bold, monospaced });
    return static_cast<FontId>(pending_.size()) - 1;
}

const Font& FontAtlas::font(FontId id) const {
    static const Font empty;
    if (id < 0 || id >= static_cast<FontId>(fonts_.size())) return empty;
    return fonts_[static_cast<std::size_t>(id)];
}

bool FontAtlas::packRect(int w, int h, int& outX, int& outY) {
    if (w > width_) return false;

    if (shelfX_ + w > width_) {
        // Start a new shelf below the tallest glyph on the current one.
        shelfX_ = 0;
        shelfY_ += shelfHeight_;
        shelfHeight_ = 0;
    }
    if (shelfY_ + h > height_) return false;

    outX = shelfX_;
    outY = shelfY_;
    shelfX_ += w;
    shelfHeight_ = std::max(shelfHeight_, h);
    return true;
}

void FontAtlas::reserveWhiteBlock() {
    int x = 0, y = 0;
    packRect(kWhiteBlockSize, kWhiteBlockSize, x, y);

    for (int row = 0; row < kWhiteBlockSize; ++row)
        for (int column = 0; column < kWhiteBlockSize; ++column)
            pixels_[static_cast<std::size_t>(y + row) * static_cast<std::size_t>(width_)
                    + static_cast<std::size_t>(x + column)] = 0xFFFFFFFFu;

    // Sample the middle of the block so bilinear filtering can never pick up a
    // neighbouring glyph's coverage.
    whiteUv_ = Vec2{ (static_cast<float>(x) + kWhiteBlockSize * 0.5f) / static_cast<float>(width_),
                     (static_cast<float>(y) + kWhiteBlockSize * 0.5f) / static_cast<float>(height_) };
}

bool FontAtlas::build() {
    if (pending_.empty()) return false;

    HDC screenDc = ::GetDC(nullptr);
    HDC dc = ::CreateCompatibleDC(screenDc);
    ::ReleaseDC(nullptr, screenDc);
    if (!dc) return false;

    const std::vector<CodepointRange>& ranges = defaultCodepointRanges();

    // Grow the atlas until every glyph fits. Starting small and doubling keeps
    // the common case at 512x512 rather than always paying for the worst case.
    for (int attemptSize = kInitialAtlasSize; attemptSize <= kMaxAtlasSize; attemptSize *= 2) {
        width_ = height_ = attemptSize;
        pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0u);
        shelfX_ = shelfY_ = shelfHeight_ = 0;
        fonts_.clear();
        fonts_.reserve(pending_.size());

        reserveWhiteBlock();

        bool overflowed = false;

        for (const PendingFont& request : pending_) {
            Font font;
            font.family = request.family;
            font.pixelHeight = request.pixelHeight;
            font.bold = request.bold;
            font.monospaced = request.monospaced;

            // A negative height asks GDI for a cell whose *character* height is
            // the value given, which is what a UI wants; a positive one includes
            // internal leading and comes out noticeably larger than requested.
            const std::wstring wideFamily = utf8ToWide(request.family);
            HFONT gdiFont = ::CreateFontW(
                -static_cast<int>(request.pixelHeight + 0.5f), 0, 0, 0,
                request.bold ? FW_SEMIBOLD : FW_NORMAL,
                FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY,
                (request.monospaced ? FIXED_PITCH : VARIABLE_PITCH) | FF_DONTCARE,
                wideFamily.c_str());

            if (!gdiFont) { overflowed = true; break; }

            HGDIOBJ previous = ::SelectObject(dc, gdiFont);

            TEXTMETRICW metrics{};
            ::GetTextMetricsW(dc, &metrics);
            font.ascent = static_cast<float>(metrics.tmAscent);
            font.descent = static_cast<float>(metrics.tmDescent);
            font.lineHeight = static_cast<float>(metrics.tmHeight + metrics.tmExternalLeading);
            if (request.monospaced) font.fixedAdvance = static_cast<float>(metrics.tmAveCharWidth);

            const MAT2 identity{ { 0, 1 }, { 0, 0 }, { 0, 0 }, { 0, 1 } };
            std::vector<std::uint8_t> glyphBuffer;

            for (const CodepointRange& range : ranges) {
                for (std::uint32_t codepoint = range.first; codepoint <= range.last; ++codepoint) {
                    GLYPHMETRICS glyphMetrics{};

                    const DWORD size = ::GetGlyphOutlineW(dc, codepoint, GGO_GRAY8_BITMAP,
                                                          &glyphMetrics, 0, nullptr, &identity);
                    if (size == GDI_ERROR) continue;

                    Glyph glyph;
                    glyph.advance = static_cast<float>(glyphMetrics.gmCellIncX);
                    glyph.offsetX = static_cast<float>(glyphMetrics.gmptGlyphOrigin.x);
                    // GDI measures the glyph origin up from the baseline; the
                    // draw list works down from the top of the line.
                    glyph.offsetY = font.ascent - static_cast<float>(glyphMetrics.gmptGlyphOrigin.y);
                    glyph.width = static_cast<float>(glyphMetrics.gmBlackBoxX);
                    glyph.height = static_cast<float>(glyphMetrics.gmBlackBoxY);

                    // Whitespace has no bitmap but still advances.
                    if (size == 0 || glyphMetrics.gmBlackBoxX == 0 || glyphMetrics.gmBlackBoxY == 0) {
                        glyph.width = glyph.height = 0.0f;
                        if (codepoint < 128) {
                            font.ascii[codepoint] = glyph;
                            font.asciiPresent[codepoint] = true;
                        } else {
                            font.extended[codepoint] = glyph;
                        }
                        continue;
                    }

                    glyphBuffer.resize(size);
                    if (::GetGlyphOutlineW(dc, codepoint, GGO_GRAY8_BITMAP, &glyphMetrics,
                                           size, glyphBuffer.data(), &identity) == GDI_ERROR)
                        continue;

                    const int glyphWidth = static_cast<int>(glyphMetrics.gmBlackBoxX);
                    const int glyphHeight = static_cast<int>(glyphMetrics.gmBlackBoxY);
                    // GGO_GRAY8 rows are padded to a DWORD boundary.
                    const int stride = (glyphWidth + 3) & ~3;

                    int atlasX = 0, atlasY = 0;
                    if (!packRect(glyphWidth + kGlyphPadding, glyphHeight + kGlyphPadding,
                                  atlasX, atlasY)) {
                        overflowed = true;
                        break;
                    }

                    for (int row = 0; row < glyphHeight; ++row) {
                        for (int column = 0; column < glyphWidth; ++column) {
                            const int coverage = glyphBuffer[static_cast<std::size_t>(row) * stride + column];
                            // 0..64 to 0..255, saturating: GDI occasionally
                            // reports 64 exactly, which must map to full opacity.
                            const std::uint32_t alpha =
                                static_cast<std::uint32_t>(std::min(255, coverage * 255 / kGrayLevels));
                            pixels_[static_cast<std::size_t>(atlasY + row) * static_cast<std::size_t>(width_)
                                    + static_cast<std::size_t>(atlasX + column)] =
                                0x00FFFFFFu | (alpha << 24);
                        }
                    }

                    glyph.u0 = static_cast<float>(atlasX) / static_cast<float>(width_);
                    glyph.v0 = static_cast<float>(atlasY) / static_cast<float>(height_);
                    glyph.u1 = static_cast<float>(atlasX + glyphWidth) / static_cast<float>(width_);
                    glyph.v1 = static_cast<float>(atlasY + glyphHeight) / static_cast<float>(height_);

                    if (codepoint < 128) {
                        font.ascii[codepoint] = glyph;
                        font.asciiPresent[codepoint] = true;
                    } else {
                        font.extended[codepoint] = glyph;
                    }
                }
                if (overflowed) break;
            }

            ::SelectObject(dc, previous);
            ::DeleteObject(gdiFont);

            if (overflowed) break;
            fonts_.push_back(std::move(font));
        }

        if (!overflowed) {
            ::DeleteDC(dc);
            built_ = true;
            return true;
        }
    }

    ::DeleteDC(dc);
    return false;
}

void FontAtlas::releasePixels() {
    pixels_.clear();
    pixels_.shrink_to_fit();
}

} // namespace acm::gfx

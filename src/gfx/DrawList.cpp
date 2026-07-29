#include "DrawList.h"

#include "FontAtlas.h"

#include <algorithm>
#include <cmath>

namespace acm::gfx {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = kPi * 2.0f;

// Width of the feathered fringe on filled shapes, in pixels.
constexpr float kFringe = 1.0f;

// Picks a segment count that keeps the error under about a third of a pixel.
int autoSegments(float radius) {
    const int segments = static_cast<int>(std::ceil(kTwoPi / std::acos(
        std::max(0.0f, std::min(1.0f, 1.0f - 0.3f / std::max(radius, 0.5f))))));
    return clampValue(segments, 8, 96);
}

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

DrawList::DrawList() {
    vertices_.reserve(8192);
    indices_.reserve(16384);
    commands_.reserve(64);
}

void DrawList::beginFrame(Vec2 displaySize) {
    displaySize_ = displaySize;
    clear();
}

void DrawList::clear() {
    vertices_.clear();
    indices_.clear();
    commands_.clear();
    clipStack_.clear();
    textureStack_.clear();
    alphaStack_.clear();
    clipStack_.push_back(Rect{ 0.0f, 0.0f, displaySize_.x, displaySize_.y });
    textureStack_.push_back(defaultTexture_);
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

Rect DrawList::currentClip() const {
    return clipStack_.empty() ? Rect{ 0.0f, 0.0f, displaySize_.x, displaySize_.y } : clipStack_.back();
}

void DrawList::pushClip(const Rect& rect, bool intersectWithCurrent) {
    clipStack_.push_back(intersectWithCurrent ? currentClip().intersected(rect) : rect);
}

void DrawList::popClip() {
    if (clipStack_.size() > 1) clipStack_.pop_back();
}

void DrawList::pushTexture(TextureId texture) {
    textureStack_.push_back(texture);
}

void DrawList::popTexture() {
    if (textureStack_.size() > 1) textureStack_.pop_back();
}

void DrawList::pushAlpha(float alpha) {
    alphaStack_.push_back(currentAlpha() * clampValue(alpha, 0.0f, 1.0f));
}

void DrawList::popAlpha() {
    if (!alphaStack_.empty()) alphaStack_.pop_back();
}

std::uint32_t DrawList::applyAlpha(const Colour& colour) const {
    const float alpha = currentAlpha();
    return alpha >= 1.0f ? colour.packed() : colour.scaledAlpha(alpha).packed();
}

// ---------------------------------------------------------------------------
// Buffer plumbing
// ---------------------------------------------------------------------------

void DrawList::ensureCommand() {
    const Rect clip = currentClip();
    const TextureId texture = textureStack_.empty() ? defaultTexture_ : textureStack_.back();

    if (!commands_.empty()) {
        DrawCommand& last = commands_.back();
        // Extending the previous command is what keeps a whole frame down to a
        // handful of draw calls.
        if (last.texture == texture
            && last.clip.x == clip.x && last.clip.y == clip.y
            && last.clip.width == clip.width && last.clip.height == clip.height)
            return;

        if (last.indexCount == 0) {
            last.clip = clip;
            last.texture = texture;
            return;
        }
    }

    DrawCommand command;
    command.clip = clip;
    command.texture = texture;
    command.indexOffset = static_cast<std::uint32_t>(indices_.size());
    command.indexCount = 0;
    commands_.push_back(command);
}

void DrawList::reserve(std::size_t vertexCount, std::size_t indexCount) {
    ensureCommand();
    vertices_.reserve(vertices_.size() + vertexCount);
    indices_.reserve(indices_.size() + indexCount);
}

std::uint32_t DrawList::appendVertex(Vec2 position, Vec2 uv, std::uint32_t colour) {
    vertices_.push_back(Vertex{ position.x, position.y, uv.x, uv.y, colour });
    return static_cast<std::uint32_t>(vertices_.size()) - 1;
}

void DrawList::appendIndex(std::uint32_t index) {
    indices_.push_back(index);
    if (!commands_.empty()) ++commands_.back().indexCount;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

void DrawList::buildRoundedRectPath(const Rect& rect, float rounding, Corners corners,
                                    std::vector<Vec2>& out) const {
    out.clear();

    // Never round more than half the shorter side, or the corners cross over.
    rounding = std::min(rounding, std::min(rect.width, rect.height) * 0.5f);

    if (rounding <= 0.5f || corners == Corners::None) {
        out.push_back({ rect.left(), rect.top() });
        out.push_back({ rect.right(), rect.top() });
        out.push_back({ rect.right(), rect.bottom() });
        out.push_back({ rect.left(), rect.bottom() });
        return;
    }

    const int segments = clampValue(static_cast<int>(rounding * 0.6f) + 3, 3, 16);

    const auto arc = [&](Vec2 centre, float startAngle, float endAngle) {
        for (int i = 0; i <= segments; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float angle = startAngle + (endAngle - startAngle) * t;
            out.push_back({ centre.x + std::cos(angle) * rounding,
                            centre.y + std::sin(angle) * rounding });
        }
    };

    // Clockwise from the top-left, in screen coordinates (y down).
    if (hasCorner(corners, Corners::TopLeft))
        arc({ rect.left() + rounding, rect.top() + rounding }, kPi, kPi * 1.5f);
    else
        out.push_back({ rect.left(), rect.top() });

    if (hasCorner(corners, Corners::TopRight))
        arc({ rect.right() - rounding, rect.top() + rounding }, kPi * 1.5f, kTwoPi);
    else
        out.push_back({ rect.right(), rect.top() });

    if (hasCorner(corners, Corners::BottomRight))
        arc({ rect.right() - rounding, rect.bottom() - rounding }, 0.0f, kPi * 0.5f);
    else
        out.push_back({ rect.right(), rect.bottom() });

    if (hasCorner(corners, Corners::BottomLeft))
        arc({ rect.left() + rounding, rect.bottom() - rounding }, kPi * 0.5f, kPi);
    else
        out.push_back({ rect.left(), rect.bottom() });
}

void DrawList::addConvexPolyFilled(const Vec2* points, int count, const Colour& colour) {
    if (count < 3) return;

    const std::uint32_t packed = applyAlpha(colour);
    const std::uint32_t transparent = applyAlpha(colour.withAlpha(0.0f));

    reserve(static_cast<std::size_t>(count) * 2, static_cast<std::size_t>(count) * 9);

    // Inner fan plus a feathered outer ring. The ring is what makes edges look
    // smooth without multisampling.
    const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());

    // Centroid, used to push the fringe outwards.
    Vec2 centroid{ 0.0f, 0.0f };
    for (int i = 0; i < count; ++i) centroid += points[i];
    centroid = centroid / static_cast<float>(count);

    for (int i = 0; i < count; ++i) {
        const Vec2 outward = (points[i] - centroid).normalised();
        appendVertex(points[i], whiteUv_, packed);
        appendVertex(points[i] + outward * kFringe, whiteUv_, transparent);
    }

    // Solid interior as a triangle fan over the inner ring.
    for (int i = 2; i < count; ++i) {
        appendIndex(base);
        appendIndex(base + static_cast<std::uint32_t>((i - 1) * 2));
        appendIndex(base + static_cast<std::uint32_t>(i * 2));
    }

    // Fringe quads.
    for (int i = 0; i < count; ++i) {
        const auto current = static_cast<std::uint32_t>(i * 2);
        const auto next = static_cast<std::uint32_t>(((i + 1) % count) * 2);

        appendIndex(base + current);
        appendIndex(base + current + 1);
        appendIndex(base + next + 1);

        appendIndex(base + current);
        appendIndex(base + next + 1);
        appendIndex(base + next);
    }
}

void DrawList::addPolyline(const Vec2* points, int count, const Colour& colour,
                           bool closed, float thickness) {
    if (count < 2 || thickness <= 0.0f) return;

    const std::uint32_t packed = applyAlpha(colour);
    const std::uint32_t transparent = applyAlpha(colour.withAlpha(0.0f));
    const float half = std::max(thickness, 0.5f) * 0.5f;

    const int segmentCount = closed ? count : count - 1;
    reserve(static_cast<std::size_t>(segmentCount) * 8, static_cast<std::size_t>(segmentCount) * 18);

    for (int i = 0; i < segmentCount; ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[(i + 1) % count];
        const Vec2 direction = (b - a).normalised();
        if (direction.x == 0.0f && direction.y == 0.0f) continue;

        const Vec2 normal = direction.perpendicular();
        const Vec2 inner = normal * half;
        const Vec2 outer = normal * (half + kFringe);

        const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());

        appendVertex(a + outer, whiteUv_, transparent);
        appendVertex(a + inner, whiteUv_, packed);
        appendVertex(a - inner, whiteUv_, packed);
        appendVertex(a - outer, whiteUv_, transparent);
        appendVertex(b + outer, whiteUv_, transparent);
        appendVertex(b + inner, whiteUv_, packed);
        appendVertex(b - inner, whiteUv_, packed);
        appendVertex(b - outer, whiteUv_, transparent);

        // Three quads: the feathered edge, the solid core, the other edge.
        const std::uint32_t quads[3][4] = {
            { 0, 1, 5, 4 },
            { 1, 2, 6, 5 },
            { 2, 3, 7, 6 },
        };
        for (const auto& quad : quads) {
            appendIndex(base + quad[0]);
            appendIndex(base + quad[1]);
            appendIndex(base + quad[2]);
            appendIndex(base + quad[0]);
            appendIndex(base + quad[2]);
            appendIndex(base + quad[3]);
        }
    }
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void DrawList::addLine(Vec2 a, Vec2 b, const Colour& colour, float thickness) {
    const Vec2 points[2] = { a, b };
    addPolyline(points, 2, colour, false, thickness);
}

void DrawList::addRectFilled(const Rect& rect, const Colour& colour,
                             float rounding, Corners corners) {
    if (rect.empty() || colour.a <= 0.0f) return;

    if (rounding <= 0.5f || corners == Corners::None) {
        // The common case: four vertices, no path building.
        const std::uint32_t packed = applyAlpha(colour);
        reserve(4, 6);
        const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
        appendVertex({ rect.left(), rect.top() }, whiteUv_, packed);
        appendVertex({ rect.right(), rect.top() }, whiteUv_, packed);
        appendVertex({ rect.right(), rect.bottom() }, whiteUv_, packed);
        appendVertex({ rect.left(), rect.bottom() }, whiteUv_, packed);
        appendIndex(base); appendIndex(base + 1); appendIndex(base + 2);
        appendIndex(base); appendIndex(base + 2); appendIndex(base + 3);
        return;
    }

    buildRoundedRectPath(rect, rounding, corners, pathScratch_);
    addConvexPolyFilled(pathScratch_.data(), static_cast<int>(pathScratch_.size()), colour);
}

void DrawList::addRect(const Rect& rect, const Colour& colour, float thickness,
                       float rounding, Corners corners) {
    if (rect.empty() || colour.a <= 0.0f) return;

    buildRoundedRectPath(rect.deflated(thickness * 0.5f), std::max(0.0f, rounding - thickness * 0.5f),
                         corners, pathScratch_);
    addPolyline(pathScratch_.data(), static_cast<int>(pathScratch_.size()), colour, true, thickness);
}

void DrawList::addRectFilledGradient(const Rect& rect, const Colour& top, const Colour& bottom,
                                     float rounding, Corners corners) {
    if (rect.empty()) return;

    if (rounding > 0.5f && corners != Corners::None) {
        // Rounded gradients are rare enough that approximating with a flat fill
        // of the average, plus the gradient inside, is not worth the vertices:
        // draw the rounded shape in the top colour and overlay the gradient
        // clipped to it.
        addRectFilled(rect, top, rounding, corners);
        pushClip(rect);
    }

    const std::uint32_t topPacked = applyAlpha(top);
    const std::uint32_t bottomPacked = applyAlpha(bottom);

    reserve(4, 6);
    const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
    const Rect inner = (rounding > 0.5f) ? rect.deflated(rounding * 0.5f) : rect;

    appendVertex({ inner.left(), inner.top() }, whiteUv_, topPacked);
    appendVertex({ inner.right(), inner.top() }, whiteUv_, topPacked);
    appendVertex({ inner.right(), inner.bottom() }, whiteUv_, bottomPacked);
    appendVertex({ inner.left(), inner.bottom() }, whiteUv_, bottomPacked);
    appendIndex(base); appendIndex(base + 1); appendIndex(base + 2);
    appendIndex(base); appendIndex(base + 2); appendIndex(base + 3);

    if (rounding > 0.5f && corners != Corners::None) popClip();
}

void DrawList::addRectFilledGradientH(const Rect& rect, const Colour& left, const Colour& right) {
    if (rect.empty()) return;

    const std::uint32_t leftPacked = applyAlpha(left);
    const std::uint32_t rightPacked = applyAlpha(right);

    reserve(4, 6);
    const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
    appendVertex({ rect.left(), rect.top() }, whiteUv_, leftPacked);
    appendVertex({ rect.right(), rect.top() }, whiteUv_, rightPacked);
    appendVertex({ rect.right(), rect.bottom() }, whiteUv_, rightPacked);
    appendVertex({ rect.left(), rect.bottom() }, whiteUv_, leftPacked);
    appendIndex(base); appendIndex(base + 1); appendIndex(base + 2);
    appendIndex(base); appendIndex(base + 2); appendIndex(base + 3);
}

void DrawList::addTriangleFilled(Vec2 a, Vec2 b, Vec2 c, const Colour& colour) {
    const Vec2 points[3] = { a, b, c };
    addConvexPolyFilled(points, 3, colour);
}

void DrawList::addCircleFilled(Vec2 centre, float radius, const Colour& colour, int segments) {
    if (radius <= 0.0f) return;
    if (segments <= 0) segments = autoSegments(radius);

    pathScratch2_.clear();
    for (int i = 0; i < segments; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        pathScratch2_.push_back({ centre.x + std::cos(angle) * radius,
                                  centre.y + std::sin(angle) * radius });
    }
    addConvexPolyFilled(pathScratch2_.data(), static_cast<int>(pathScratch2_.size()), colour);
}

void DrawList::addCircle(Vec2 centre, float radius, const Colour& colour,
                         float thickness, int segments) {
    if (radius <= 0.0f) return;
    if (segments <= 0) segments = autoSegments(radius);

    pathScratch2_.clear();
    for (int i = 0; i < segments; ++i) {
        const float angle = kTwoPi * static_cast<float>(i) / static_cast<float>(segments);
        pathScratch2_.push_back({ centre.x + std::cos(angle) * radius,
                                  centre.y + std::sin(angle) * radius });
    }
    addPolyline(pathScratch2_.data(), static_cast<int>(pathScratch2_.size()), colour, true, thickness);
}

void DrawList::addArc(Vec2 centre, float radius, float startAngle, float endAngle,
                      const Colour& colour, float thickness, int segments) {
    if (radius <= 0.0f) return;
    if (segments <= 0)
        segments = clampValue(static_cast<int>(std::fabs(endAngle - startAngle) * radius * 0.4f), 4, 96);

    pathScratch2_.clear();
    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = startAngle + (endAngle - startAngle) * t;
        pathScratch2_.push_back({ centre.x + std::cos(angle) * radius,
                                  centre.y + std::sin(angle) * radius });
    }
    addPolyline(pathScratch2_.data(), static_cast<int>(pathScratch2_.size()), colour, false, thickness);
}

void DrawList::addBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, const Colour& colour,
                         float thickness, int segments) {
    if (segments <= 0) {
        // More segments for longer cables, so a cable across the canvas is still
        // smooth without paying for it on short ones.
        const float roughLength = (p3 - p0).length() + (p1 - p0).length() + (p3 - p2).length();
        segments = clampValue(static_cast<int>(roughLength * 0.08f), 12, 64);
    }

    pathScratch2_.clear();
    pathScratch2_.reserve(static_cast<std::size_t>(segments) + 1);

    for (int i = 0; i <= segments; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(segments);
        const float u = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;
        pathScratch2_.push_back({ p0.x * w0 + p1.x * w1 + p2.x * w2 + p3.x * w3,
                                  p0.y * w0 + p1.y * w1 + p2.y * w2 + p3.y * w3 });
    }

    addPolyline(pathScratch2_.data(), static_cast<int>(pathScratch2_.size()), colour, false, thickness);
}

void DrawList::addGlow(const Rect& rect, const Colour& colour, float radius,
                       float rounding, int steps) {
    if (radius <= 0.0f || steps <= 0) return;

    // Concentric outlines whose alpha falls off quadratically. Cheap, and it
    // reads as a soft bloom against the near-black background.
    for (int i = steps; i >= 1; --i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float spread = radius * t;
        const float alpha = colour.a * (1.0f - t) * (1.0f - t) * 0.6f;
        if (alpha <= 0.002f) continue;

        addRectFilled(rect.inflated(spread), colour.withAlpha(alpha),
                      rounding + spread, Corners::All);
    }
}

void DrawList::addImage(TextureId texture, const Rect& rect, Vec2 uvMin, Vec2 uvMax,
                        const Colour& tint) {
    if (rect.empty()) return;

    pushTexture(texture);
    const std::uint32_t packed = applyAlpha(tint);

    reserve(4, 6);
    const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
    appendVertex({ rect.left(), rect.top() }, { uvMin.x, uvMin.y }, packed);
    appendVertex({ rect.right(), rect.top() }, { uvMax.x, uvMin.y }, packed);
    appendVertex({ rect.right(), rect.bottom() }, { uvMax.x, uvMax.y }, packed);
    appendVertex({ rect.left(), rect.bottom() }, { uvMin.x, uvMax.y }, packed);
    appendIndex(base); appendIndex(base + 1); appendIndex(base + 2);
    appendIndex(base); appendIndex(base + 2); appendIndex(base + 3);

    popTexture();
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

void DrawList::addText(const Font& font, Vec2 position, const Colour& colour,
                       std::string_view text) {
    if (text.empty() || colour.a <= 0.0f) return;

    const std::uint32_t packed = applyAlpha(colour);
    const Rect clip = currentClip();

    reserve(text.size() * 4, text.size() * 6);

    // Snapping the pen to whole pixels keeps stems crisp; sub-pixel positioning
    // would blur every label at this size.
    float penX = std::floor(position.x + 0.5f);
    const float penY = std::floor(position.y + 0.5f);

    std::size_t index = 0;
    while (index < text.size()) {
        const std::uint32_t codepoint = nextCodepoint(text, index);
        if (codepoint == '\n') continue;

        const Glyph* glyph = font.glyph(codepoint);
        if (!glyph) glyph = font.glyph(U'?');
        if (!glyph) continue;

        if (glyph->width > 0.0f && glyph->height > 0.0f) {
            const float x0 = penX + glyph->offsetX;
            const float y0 = penY + glyph->offsetY;
            const float x1 = x0 + glyph->width;
            const float y1 = y0 + glyph->height;

            // Skip glyphs entirely outside the clip: a long string in a narrow
            // column should not cost vertices for the part nobody sees.
            if (x1 >= clip.left() && x0 <= clip.right()) {
                const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
                appendVertex({ x0, y0 }, { glyph->u0, glyph->v0 }, packed);
                appendVertex({ x1, y0 }, { glyph->u1, glyph->v0 }, packed);
                appendVertex({ x1, y1 }, { glyph->u1, glyph->v1 }, packed);
                appendVertex({ x0, y1 }, { glyph->u0, glyph->v1 }, packed);
                appendIndex(base); appendIndex(base + 1); appendIndex(base + 2);
                appendIndex(base); appendIndex(base + 2); appendIndex(base + 3);
            }
        }

        penX += glyph->advance;
        if (penX > clip.right() + 64.0f) break;   // the rest is off-screen
    }
}

void DrawList::addTextClipped(const Font& font, const Rect& rect, const Colour& colour,
                              std::string_view text, Align horizontal, bool verticallyCentred) {
    if (text.empty() || rect.empty()) return;

    std::string truncated;
    std::string_view toDraw = text;
    float width = font.textWidth(text);

    if (width > rect.width) {
        // Ellipsis rather than a hard cut, so a truncated plugin name still
        // reads as a truncated name.
        const std::string_view ellipsis = "\xE2\x80\xA6";
        const float ellipsisWidth = font.textWidth(ellipsis);
        const std::size_t fitting = font.prefixFitting(text, std::max(0.0f, rect.width - ellipsisWidth));

        truncated.assign(text.substr(0, fitting));
        truncated.append(ellipsis);
        toDraw = truncated;
        width = font.textWidth(toDraw);
    }

    float x = rect.left();
    if (horizontal == Align::Centre) x = rect.left() + (rect.width - width) * 0.5f;
    else if (horizontal == Align::Right) x = rect.right() - width;

    const float y = verticallyCentred
                        ? rect.top() + (rect.height - font.lineHeight) * 0.5f
                        : rect.top();

    pushClip(rect);
    addText(font, { x, y }, colour, toDraw);
    popClip();
}

} // namespace acm::gfx

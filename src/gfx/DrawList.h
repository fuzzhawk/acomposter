// Immediate-mode geometry buffer.
//
// The UI never talks to Direct3D. It appends shapes and text to a DrawList,
// which accumulates one vertex/index buffer plus a small list of draw commands
// split wherever the clip rectangle or the bound texture changes. The renderer
// then uploads the whole frame in two buffer writes and replays the commands.
//
// Anti-aliasing is done by geometry rather than by MSAA: every filled shape gets
// a one-pixel feathered fringe, which keeps the look crisp at any window size
// and costs nothing on the GPU.
#pragma once

#include "Geometry.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace acm::gfx {

class FontAtlas;
struct Font;

using TextureId = std::uint32_t;
inline constexpr TextureId kNoTexture = 0;

struct Vertex {
    float x, y;
    float u, v;
    std::uint32_t colour;
};

struct DrawCommand {
    Rect clip;
    TextureId texture = kNoTexture;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
};

// Which corners a rounded rectangle actually rounds.
enum class Corners : std::uint32_t {
    None = 0,
    TopLeft = 1u << 0,
    TopRight = 1u << 1,
    BottomRight = 1u << 2,
    BottomLeft = 1u << 3,
    Top = TopLeft | TopRight,
    Bottom = BottomLeft | BottomRight,
    Left = TopLeft | BottomLeft,
    Right = TopRight | BottomRight,
    All = Top | Bottom,
};

constexpr Corners operator|(Corners a, Corners b) {
    return static_cast<Corners>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}
constexpr bool hasCorner(Corners set, Corners which) {
    return (static_cast<std::uint32_t>(set) & static_cast<std::uint32_t>(which)) != 0;
}

class DrawList {
public:
    DrawList();

    void beginFrame(Vec2 displaySize);
    void clear();

    // -- state -------------------------------------------------------------
    void pushClip(const Rect& rect, bool intersectWithCurrent = true);
    void popClip();
    Rect currentClip() const;

    void pushTexture(TextureId texture);
    void popTexture();

    // Everything appended afterwards is multiplied by this. Used to fade whole
    // panels in and out without touching each call site.
    void pushAlpha(float alpha);
    void popAlpha();
    float currentAlpha() const { return alphaStack_.empty() ? 1.0f : alphaStack_.back(); }

    // -- primitives --------------------------------------------------------
    void addLine(Vec2 a, Vec2 b, const Colour& colour, float thickness = 1.0f);
    void addRect(const Rect& rect, const Colour& colour, float thickness = 1.0f,
                 float rounding = 0.0f, Corners corners = Corners::All);
    void addRectFilled(const Rect& rect, const Colour& colour,
                       float rounding = 0.0f, Corners corners = Corners::All);
    void addRectFilledGradient(const Rect& rect, const Colour& top, const Colour& bottom,
                               float rounding = 0.0f, Corners corners = Corners::All);
    void addRectFilledGradientH(const Rect& rect, const Colour& left, const Colour& right);

    void addTriangleFilled(Vec2 a, Vec2 b, Vec2 c, const Colour& colour);
    void addCircleFilled(Vec2 centre, float radius, const Colour& colour, int segments = 0);
    void addCircle(Vec2 centre, float radius, const Colour& colour,
                   float thickness = 1.0f, int segments = 0);
    // Filled pie slice, used for knob value arcs. Angles in radians, clockwise
    // from the positive x axis.
    void addArc(Vec2 centre, float radius, float startAngle, float endAngle,
                const Colour& colour, float thickness, int segments = 0);

    void addPolyline(const Vec2* points, int count, const Colour& colour,
                     bool closed, float thickness);
    void addConvexPolyFilled(const Vec2* points, int count, const Colour& colour);

    // Cubic Bezier, which is what patch cables are drawn with.
    void addBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, const Colour& colour,
                   float thickness, int segments = 0);

    // A soft rectangular glow, drawn as concentric fading outlines. The dark lab
    // look leans on this for anything that is live or selected.
    void addGlow(const Rect& rect, const Colour& colour, float radius,
                 float rounding = 0.0f, int steps = 6);

    void addImage(TextureId texture, const Rect& rect,
                  Vec2 uvMin = { 0.0f, 0.0f }, Vec2 uvMax = { 1.0f, 1.0f },
                  const Colour& tint = Colour{ 1.0f, 1.0f, 1.0f, 1.0f });

    // -- text --------------------------------------------------------------
    void addText(const Font& font, Vec2 position, const Colour& colour, std::string_view text);
    // Draws inside `rect`, aligned. Truncates with an ellipsis when too long.
    enum class Align { Left, Centre, Right };
    void addTextClipped(const Font& font, const Rect& rect, const Colour& colour,
                        std::string_view text, Align horizontal = Align::Left,
                        bool verticallyCentred = true);

    // -- output ------------------------------------------------------------
    const std::vector<Vertex>& vertices() const noexcept { return vertices_; }
    const std::vector<std::uint32_t>& indices() const noexcept { return indices_; }
    const std::vector<DrawCommand>& commands() const noexcept { return commands_; }
    Vec2 displaySize() const noexcept { return displaySize_; }

    // The atlas supplies the solid-white texel every untextured shape samples,
    // so one shader and one texture binding cover the whole frame.
    void setWhitePixelUv(Vec2 uv) noexcept { whiteUv_ = uv; }
    void setDefaultTexture(TextureId texture) noexcept { defaultTexture_ = texture; }

private:
    void ensureCommand();
    void reserve(std::size_t vertexCount, std::size_t indexCount);
    std::uint32_t appendVertex(Vec2 position, Vec2 uv, std::uint32_t colour);
    void appendIndex(std::uint32_t index);
    std::uint32_t applyAlpha(const Colour& colour) const;

    // Builds the outline of a rounded rectangle into `out`.
    void buildRoundedRectPath(const Rect& rect, float rounding, Corners corners,
                              std::vector<Vec2>& out) const;

    std::vector<Vertex> vertices_;
    std::vector<std::uint32_t> indices_;
    std::vector<DrawCommand> commands_;

    std::vector<Rect> clipStack_;
    std::vector<TextureId> textureStack_;
    std::vector<float> alphaStack_;

    Vec2 displaySize_{ 0.0f, 0.0f };
    Vec2 whiteUv_{ 0.0f, 0.0f };
    TextureId defaultTexture_ = kNoTexture;

    // Scratch reused across calls so building a frame does not churn the heap.
    mutable std::vector<Vec2> pathScratch_;
    mutable std::vector<Vec2> pathScratch2_;
};

} // namespace acm::gfx

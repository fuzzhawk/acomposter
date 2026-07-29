// 2D geometry and colour, shared by the renderer and the UI layer.
#pragma once

#include "../core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace acm::gfx {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr Vec2() = default;
    constexpr Vec2(float x, float y) : x(x), y(y) {}

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return { a.x + b.x, a.y + b.y }; }
    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return { a.x - b.x, a.y - b.y }; }
    friend constexpr Vec2 operator*(Vec2 a, float s) { return { a.x * s, a.y * s }; }
    friend constexpr Vec2 operator*(float s, Vec2 a) { return { a.x * s, a.y * s }; }
    friend constexpr Vec2 operator/(Vec2 a, float s) { return { a.x / s, a.y / s }; }
    Vec2& operator+=(Vec2 b) { x += b.x; y += b.y; return *this; }
    Vec2& operator-=(Vec2 b) { x -= b.x; y -= b.y; return *this; }
    friend constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSquared() const { return x * x + y * y; }

    Vec2 normalised() const {
        const float l = length();
        return l > 1.0e-6f ? Vec2{ x / l, y / l } : Vec2{ 0.0f, 0.0f };
    }
    // Rotated 90 degrees, for building line quads.
    constexpr Vec2 perpendicular() const { return { -y, x }; }
};

inline Vec2 lerp(Vec2 a, Vec2 b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

struct Rect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;

    constexpr Rect() = default;
    constexpr Rect(float x, float y, float w, float h) : x(x), y(y), width(w), height(h) {}

    static constexpr Rect fromCorners(Vec2 a, Vec2 b) {
        const float left = a.x < b.x ? a.x : b.x;
        const float top = a.y < b.y ? a.y : b.y;
        const float right = a.x > b.x ? a.x : b.x;
        const float bottom = a.y > b.y ? a.y : b.y;
        return { left, top, right - left, bottom - top };
    }

    constexpr float left() const { return x; }
    constexpr float top() const { return y; }
    constexpr float right() const { return x + width; }
    constexpr float bottom() const { return y + height; }
    constexpr Vec2 topLeft() const { return { x, y }; }
    constexpr Vec2 bottomRight() const { return { x + width, y + height }; }
    constexpr Vec2 centre() const { return { x + width * 0.5f, y + height * 0.5f }; }
    constexpr Vec2 size() const { return { width, height }; }

    constexpr bool empty() const { return width <= 0.0f || height <= 0.0f; }

    constexpr bool contains(Vec2 p) const {
        return p.x >= x && p.x < x + width && p.y >= y && p.y < y + height;
    }

    constexpr bool intersects(const Rect& other) const {
        return !(other.x >= right() || other.right() <= x
              || other.y >= bottom() || other.bottom() <= y);
    }

    constexpr Rect inflated(float amount) const {
        return { x - amount, y - amount, width + amount * 2.0f, height + amount * 2.0f };
    }
    constexpr Rect deflated(float amount) const { return inflated(-amount); }

    constexpr Rect translated(Vec2 delta) const { return { x + delta.x, y + delta.y, width, height }; }

    Rect intersected(const Rect& other) const {
        const float left = std::max(x, other.x);
        const float top = std::max(y, other.y);
        const float right = std::min(this->right(), other.right());
        const float bottom = std::min(this->bottom(), other.bottom());
        return { left, top, std::max(0.0f, right - left), std::max(0.0f, bottom - top) };
    }

    // Splits a strip off one edge and shrinks this rect by it. The workhorse of
    // the layout code.
    Rect removeFromTop(float amount) {
        const float taken = std::min(amount, height);
        const Rect result{ x, y, width, taken };
        y += taken;
        height -= taken;
        return result;
    }
    Rect removeFromBottom(float amount) {
        const float taken = std::min(amount, height);
        height -= taken;
        return { x, y + height, width, taken };
    }
    Rect removeFromLeft(float amount) {
        const float taken = std::min(amount, width);
        const Rect result{ x, y, taken, height };
        x += taken;
        width -= taken;
        return result;
    }
    Rect removeFromRight(float amount) {
        const float taken = std::min(amount, width);
        width -= taken;
        return { x + width, y, taken, height };
    }

    Rect withHeight(float h) const { return { x, y, width, h }; }
    Rect withWidth(float w) const { return { x, y, w, height }; }
};

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

struct Colour {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

    constexpr Colour() = default;
    constexpr Colour(float r, float g, float b, float a = 1.0f) : r(r), g(g), b(b), a(a) {}

    // 0xRRGGBB or 0xAARRGGBB.
    static constexpr Colour fromHex(std::uint32_t hex, float alpha = 1.0f) {
        return Colour{ static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
                       static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
                       static_cast<float>(hex & 0xFF) / 255.0f,
                       alpha };
    }

    static constexpr Colour fromArgb(std::uint32_t argb) {
        return Colour{ static_cast<float>((argb >> 16) & 0xFF) / 255.0f,
                       static_cast<float>((argb >> 8) & 0xFF) / 255.0f,
                       static_cast<float>(argb & 0xFF) / 255.0f,
                       static_cast<float>((argb >> 24) & 0xFF) / 255.0f };
    }

    constexpr Colour withAlpha(float alpha) const { return { r, g, b, alpha }; }
    constexpr Colour scaledAlpha(float factor) const { return { r, g, b, a * factor }; }

    // Multiplies the RGB, leaving alpha alone: how the UI derives hover and
    // pressed states from one base colour.
    constexpr Colour brightened(float factor) const {
        return { r * factor > 1.0f ? 1.0f : r * factor,
                 g * factor > 1.0f ? 1.0f : g * factor,
                 b * factor > 1.0f ? 1.0f : b * factor, a };
    }

    // The renderer's vertex format: R8G8B8A8 in memory, so the low byte is red.
    std::uint32_t packed() const {
        const auto quantise = [](float v) -> std::uint32_t {
            const float clamped = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
        };
        return quantise(r) | (quantise(g) << 8) | (quantise(b) << 16) | (quantise(a) << 24);
    }

    float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
};

inline Colour lerp(const Colour& a, const Colour& b, float t) {
    return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
             a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t };
}

// Blends `over` on top of `under` using `over`'s alpha.
inline Colour composite(const Colour& under, const Colour& over) {
    return lerp(under, over.withAlpha(under.a), over.a);
}

} // namespace acm::gfx

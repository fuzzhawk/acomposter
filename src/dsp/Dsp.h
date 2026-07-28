// Small DSP primitives shared by the nodes.
//
// Everything here is header-only and allocation-free so it can be used freely
// inside process().
#pragma once

#include "../core/Types.h"

#include <cmath>
#include <cstring>

namespace acm::dsp {

// ---------------------------------------------------------------------------
// Gain / level
// ---------------------------------------------------------------------------

inline float dbToGain(float db) noexcept {
    return db <= -95.0f ? 0.0f : std::pow(10.0f, db * 0.05f);
}

inline float gainToDb(float gain) noexcept {
    return gain <= 1.0e-6f ? -120.0f : 20.0f * std::log10(gain);
}

// ---------------------------------------------------------------------------
// Fractional-index reads
//
// The sample player and the resampler both need to read between samples. Linear
// is cheap and fine for small rate changes; Hermite is the default because
// pitching a break down an octave with linear interpolation sounds like a
// low-pass filter with a bad attitude.
// ---------------------------------------------------------------------------

inline float interpolateLinear(const float* data, std::int64_t length, double position) noexcept {
    if (length <= 0) return 0.0f;
    const auto i0 = static_cast<std::int64_t>(position);
    if (i0 < 0 || i0 >= length) return 0.0f;
    const auto i1 = (i0 + 1 < length) ? i0 + 1 : i0;
    const float frac = static_cast<float>(position - static_cast<double>(i0));
    return data[i0] + (data[i1] - data[i0]) * frac;
}

// 4-point, 3rd-order Hermite (Catmull-Rom). Reads one sample either side, so it
// clamps at the buffer edges rather than reading out of bounds.
inline float interpolateHermite(const float* data, std::int64_t length, double position) noexcept {
    if (length <= 0) return 0.0f;
    const auto i1 = static_cast<std::int64_t>(position);
    if (i1 < 0 || i1 >= length) return 0.0f;

    const auto clampIndex = [length](std::int64_t i) noexcept -> std::int64_t {
        return i < 0 ? 0 : (i >= length ? length - 1 : i);
    };

    const float xm1 = data[clampIndex(i1 - 1)];
    const float x0 = data[i1];
    const float x1 = data[clampIndex(i1 + 1)];
    const float x2 = data[clampIndex(i1 + 2)];
    const float t = static_cast<float>(position - static_cast<double>(i1));

    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

// Same as above but wrapping at `length`, for looping reads where the loop is
// the whole buffer.
inline float interpolateHermiteWrapped(const float* data, std::int64_t length, double position) noexcept {
    if (length <= 0) return 0.0f;
    auto i1 = static_cast<std::int64_t>(position) % length;
    if (i1 < 0) i1 += length;

    const auto wrap = [length](std::int64_t i) noexcept -> std::int64_t {
        i %= length;
        return i < 0 ? i + length : i;
    };

    const float xm1 = data[wrap(i1 - 1)];
    const float x0 = data[i1];
    const float x1 = data[wrap(i1 + 1)];
    const float x2 = data[wrap(i1 + 2)];
    const float t = static_cast<float>(position - std::floor(position));

    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    return ((a * t - b) * t + c) * t + x0;
}

// ---------------------------------------------------------------------------
// Fade and pan laws
// ---------------------------------------------------------------------------

enum class FadeLaw : int {
    ConstantPower = 0,  // -3 dB centre; keeps perceived loudness through the sweep
    Linear,             // -6 dB centre; the classic DJ-style dip
    ConstantGain,       // sums to unity, so correlated material does not build up
    Transition,         // holds both sides near full until the last stretch
};

// Fills the pair of gains for a two-source crossfade. `t` runs 0 (all A) to 1
// (all B).
inline void crossfadeGains(FadeLaw law, float t, float& gainA, float& gainB) noexcept {
    t = clampValue(t, 0.0f, 1.0f);

    switch (law) {
        case FadeLaw::ConstantPower: {
            constexpr float halfPi = 1.57079632679489661923f;
            gainA = std::cos(t * halfPi);
            gainB = std::sin(t * halfPi);
            break;
        }
        case FadeLaw::Linear:
            gainA = 1.0f - t;
            gainB = t;
            break;
        case FadeLaw::ConstantGain: {
            // Both curves stay flat-ish across the middle and only collapse at
            // the very ends, at the cost of a level bump for uncorrelated audio.
            gainA = std::sqrt(1.0f - t * t * (3.0f - 2.0f * t));
            gainB = std::sqrt(t * t * (3.0f - 2.0f * t));
            break;
        }
        case FadeLaw::Transition: {
            // Full level until 40% then a fast handover: what you want when
            // cutting between two rhythmic parts.
            const float a = clampValue((1.0f - t) * 1.6f, 0.0f, 1.0f);
            const float b = clampValue(t * 1.6f, 0.0f, 1.0f);
            gainA = std::sqrt(a);
            gainB = std::sqrt(b);
            break;
        }
    }
}

// Equal-power stereo pan. `pan` runs -1 (hard left) to +1 (hard right).
inline void panGains(float pan, float& left, float& right) noexcept {
    pan = clampValue(pan, -1.0f, 1.0f);
    const float t = (pan + 1.0f) * 0.5f;
    constexpr float halfPi = 1.57079632679489661923f;
    left = std::cos(t * halfPi);
    right = std::sin(t * halfPi);
}

// ---------------------------------------------------------------------------
// Metering
// ---------------------------------------------------------------------------

// Peak follower with an instant attack and an exponential release, so a meter
// reads true peak but stays legible.
class PeakFollower {
public:
    void prepare(double sampleRate, double releaseSeconds = 0.35) noexcept {
        const double n = sampleRate * releaseSeconds;
        release_ = n > 1.0 ? static_cast<float>(std::exp(-1.0 / n)) : 0.0f;
        value_ = 0.0f;
    }

    float process(const float* data, int frames) noexcept {
        float blockPeak = 0.0f;
        for (int i = 0; i < frames; ++i) {
            const float a = data[i] < 0.0f ? -data[i] : data[i];
            if (a > blockPeak) blockPeak = a;
        }
        // Decay once per block rather than per sample; at audio block sizes the
        // difference is inaudible and the loop stays cheap.
        value_ *= std::pow(release_, static_cast<float>(frames));
        if (blockPeak > value_) value_ = blockPeak;
        return value_;
    }

    float value() const noexcept { return value_; }
    void reset() noexcept { value_ = 0.0f; }

private:
    float value_ = 0.0f;
    float release_ = 0.99f;
};

class RmsFollower {
public:
    void prepare(double sampleRate, double windowSeconds = 0.3) noexcept {
        const double n = sampleRate * windowSeconds;
        coeff_ = n > 1.0 ? static_cast<float>(std::exp(-1.0 / n)) : 0.0f;
        accumulator_ = 0.0f;
    }

    float process(const float* data, int frames) noexcept {
        for (int i = 0; i < frames; ++i)
            accumulator_ = accumulator_ * coeff_ + data[i] * data[i] * (1.0f - coeff_);
        return std::sqrt(accumulator_);
    }

    void reset() noexcept { accumulator_ = 0.0f; }

private:
    float accumulator_ = 0.0f;
    float coeff_ = 0.99f;
};

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------

// Removes the DC offset that feedback loops and some plugins accumulate.
class DcBlocker {
public:
    void prepare(double sampleRate) noexcept {
        r_ = static_cast<float>(1.0 - (2.0 * 3.14159265358979 * 8.0 / sampleRate));
        reset();
    }
    void reset() noexcept { x1_ = y1_ = 0.0f; }

    float process(float x) noexcept {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

private:
    float r_ = 0.999f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
};

// Transposed direct form II biquad - stable under coefficient modulation, which
// matters when a filter cutoff is being swept by the metasurface.
class Biquad {
public:
    enum class Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };

    void reset() noexcept { z1_ = z2_ = 0.0f; }

    void setCoefficients(Type type, double sampleRate, double frequency,
                         double q, double gainDb = 0.0) noexcept {
        frequency = clampValue(frequency, 10.0, sampleRate * 0.49);
        q = clampValue(q, 0.05, 40.0);

        const double omega = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
        const double sn = std::sin(omega);
        const double cs = std::cos(omega);
        const double alpha = sn / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;

        switch (type) {
            case Type::LowPass:
                b0 = (1.0 - cs) * 0.5; b1 = 1.0 - cs; b2 = b0;
                a0 = 1.0 + alpha; a1 = -2.0 * cs; a2 = 1.0 - alpha;
                break;
            case Type::HighPass:
                b0 = (1.0 + cs) * 0.5; b1 = -(1.0 + cs); b2 = b0;
                a0 = 1.0 + alpha; a1 = -2.0 * cs; a2 = 1.0 - alpha;
                break;
            case Type::BandPass:
                b0 = alpha; b1 = 0.0; b2 = -alpha;
                a0 = 1.0 + alpha; a1 = -2.0 * cs; a2 = 1.0 - alpha;
                break;
            case Type::Notch:
                b0 = 1.0; b1 = -2.0 * cs; b2 = 1.0;
                a0 = 1.0 + alpha; a1 = -2.0 * cs; a2 = 1.0 - alpha;
                break;
            case Type::Peak:
                b0 = 1.0 + alpha * A; b1 = -2.0 * cs; b2 = 1.0 - alpha * A;
                a0 = 1.0 + alpha / A; a1 = -2.0 * cs; a2 = 1.0 - alpha / A;
                break;
            case Type::LowShelf: {
                const double beta = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) - (A - 1.0) * cs + beta);
                b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cs);
                b2 = A * ((A + 1.0) - (A - 1.0) * cs - beta);
                a0 = (A + 1.0) + (A - 1.0) * cs + beta;
                a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cs);
                a2 = (A + 1.0) + (A - 1.0) * cs - beta;
                break;
            }
            case Type::HighShelf: {
                const double beta = 2.0 * std::sqrt(A) * alpha;
                b0 = A * ((A + 1.0) + (A - 1.0) * cs + beta);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cs);
                b2 = A * ((A + 1.0) + (A - 1.0) * cs - beta);
                a0 = (A + 1.0) - (A - 1.0) * cs + beta;
                a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cs);
                a2 = (A + 1.0) - (A - 1.0) * cs - beta;
                break;
            }
        }

        const double inv = (a0 != 0.0) ? 1.0 / a0 : 1.0;
        b0_ = static_cast<float>(b0 * inv);
        b1_ = static_cast<float>(b1 * inv);
        b2_ = static_cast<float>(b2 * inv);
        a1_ = static_cast<float>(a1 * inv);
        a2_ = static_cast<float>(a2 * inv);
    }

    float process(float x) noexcept {
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;
        return y;
    }

private:
    float b0_ = 1.0f, b1_ = 0.0f, b2_ = 0.0f, a1_ = 0.0f, a2_ = 0.0f;
    float z1_ = 0.0f, z2_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Cheap deterministic noise, for the test-signal node
// ---------------------------------------------------------------------------

class Xorshift {
public:
    explicit Xorshift(std::uint32_t seed = 0x1234567u) : state_(seed ? seed : 1u) {}

    std::uint32_t nextUint() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    // Uniform in [-1, 1).
    float nextFloat() noexcept {
        return static_cast<float>(static_cast<std::int32_t>(nextUint())) * (1.0f / 2147483648.0f);
    }

private:
    std::uint32_t state_;
};

} // namespace acm::dsp

// A single automatable control value.
//
// Parameters are the currency of the whole application: the patcher draws them,
// the metasurface interpolates them, the patch file serialises them, and VST2
// plugins expose theirs through the same interface. Two rules make that work:
//
//   * the authoritative value lives in one atomic float, written by whichever
//     thread the user is on and read by the audio thread without a lock, and
//   * every parameter can map to and from a normalised 0..1 domain, because
//     snapshot interpolation is only musically sensible in normalised space (a
//     linear blend between 20 Hz and 20 kHz is not a filter sweep).
#pragma once

#include "Types.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace acm {

enum class ParamKind : std::uint8_t {
    Float,   // continuous
    Bool,    // 0 or 1
    Choice,  // index into choices()
    Int,     // integral steps
};

enum class ParamCurve : std::uint8_t {
    Linear,
    Skewed,     // pow curve, configured via setSkewForCentre()
    Decibels,   // stored in dB; already perceptual, so mapped linearly
};

// How the metasurface blends this parameter between snapshots. Continuous
// parameters cross-fade; stepped ones snap to the nearest-weighted snapshot,
// because a "half way between reverse and forward" playback direction is not a
// thing.
enum class ParamBlend : std::uint8_t { Continuous, Stepped };

class Parameter {
public:
    Parameter(std::string id, std::string name, ParamKind kind,
              float minValue, float maxValue, float defaultValue);

    Parameter(const Parameter&) = delete;
    Parameter& operator=(const Parameter&) = delete;

    // -- identity ----------------------------------------------------------
    const std::string& id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    ParamKind kind() const noexcept { return kind_; }

    Parameter& setUnit(std::string u) { unit_ = std::move(u); return *this; }
    const std::string& unit() const noexcept { return unit_; }

    Parameter& setChoices(std::vector<std::string> c);
    const std::vector<std::string>& choices() const noexcept { return choices_; }

    Parameter& setCurve(ParamCurve c) { curve_ = c; return *this; }
    ParamCurve curve() const noexcept { return curve_; }

    // Chooses the pow() exponent such that `centre` lands at normalised 0.5.
    Parameter& setSkewForCentre(float centre);

    Parameter& setBlend(ParamBlend b) { blend_ = b; return *this; }
    ParamBlend blend() const noexcept { return blend_; }

    // Excluded parameters are still saved with the patch but never captured by
    // snapshots - used for things like a sample player's file slot.
    Parameter& setAutomatable(bool a) { automatable_ = a; return *this; }
    bool automatable() const noexcept { return automatable_; }

    Parameter& setDescription(std::string d) { description_ = std::move(d); return *this; }
    const std::string& description() const noexcept { return description_; }

    // -- range -------------------------------------------------------------
    float minValue() const noexcept { return min_; }
    float maxValue() const noexcept { return max_; }
    float defaultValue() const noexcept { return default_; }

    // -- value (thread safe) ----------------------------------------------
    float value() const noexcept { return value_.load(std::memory_order_relaxed); }
    void setValue(float v) noexcept { value_.store(snap(clampValue(v, min_, max_)), std::memory_order_relaxed); }

    float normalised() const noexcept { return toNormalised(value()); }
    void setNormalised(float n) noexcept { setValue(fromNormalised(n)); }

    void resetToDefault() noexcept { setValue(default_); }

    bool boolValue() const noexcept { return value() >= 0.5f; }
    int intValue() const noexcept { return static_cast<int>(value() + (value() < 0.0f ? -0.5f : 0.5f)); }

    // -- mapping -----------------------------------------------------------
    float toNormalised(float v) const noexcept;
    float fromNormalised(float n) const noexcept;

    // -- display -----------------------------------------------------------
    std::string toText(float v) const;
    std::string toText() const { return toText(value()); }
    // Returns false when the text cannot be interpreted; the value is untouched.
    bool fromText(const std::string& text, float& out) const;

private:
    // Quantises to the parameter's step grid (integral kinds only).
    float snap(float v) const noexcept;

    std::string id_;
    std::string name_;
    std::string unit_;
    std::string description_;
    std::vector<std::string> choices_;

    ParamKind kind_ = ParamKind::Float;
    ParamCurve curve_ = ParamCurve::Linear;
    ParamBlend blend_ = ParamBlend::Continuous;
    bool automatable_ = true;

    float min_ = 0.0f;
    float max_ = 1.0f;
    float default_ = 0.0f;
    float skew_ = 1.0f;

    std::atomic<float> value_{ 0.0f };
};

using ParameterPtr = std::unique_ptr<Parameter>;

// ---------------------------------------------------------------------------
// A parameter that follows its target over a short ramp.
//
// Nodes hold one of these per audible control and call setTarget() once per
// block from the atomic, then next() per sample. That removes zipper noise
// without forcing the UI to do anything special.
// ---------------------------------------------------------------------------

class SmoothedValue {
public:
    void reset(double sampleRate, double rampSeconds = 0.02) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
        rampSeconds_ = rampSeconds;
        steps_ = static_cast<int>(rampSeconds_ * sampleRate_);
        if (steps_ < 1) steps_ = 1;
        countdown_ = 0;
        current_ = target_;
        increment_ = 0.0f;
    }

    // Changes the ramp duration without disturbing a ramp in progress, so a
    // node can expose its fade time as a live parameter.
    void setRampSeconds(double seconds) noexcept {
        if (seconds == rampSeconds_) return;
        rampSeconds_ = seconds;
        steps_ = static_cast<int>(rampSeconds_ * sampleRate_);
        if (steps_ < 1) steps_ = 1;
        if (countdown_ > 0) {
            countdown_ = steps_;
            increment_ = (target_ - current_) / static_cast<float>(steps_);
        }
    }

    void setCurrentAndTarget(float v) noexcept {
        current_ = target_ = v;
        countdown_ = 0;
        increment_ = 0.0f;
    }

    void setTarget(float v) noexcept {
        if (v == target_) return;
        target_ = v;
        countdown_ = steps_;
        increment_ = (target_ - current_) / static_cast<float>(steps_);
    }

    float next() noexcept {
        if (countdown_ <= 0) return current_ = target_;
        current_ += increment_;
        --countdown_;
        return current_;
    }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }
    bool smoothing() const noexcept { return countdown_ > 0; }

private:
    double sampleRate_ = 44100.0;
    double rampSeconds_ = 0.02;
    int steps_ = 1;
    int countdown_ = 0;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float increment_ = 0.0f;
};

} // namespace acm

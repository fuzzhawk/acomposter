#include "Parameter.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace acm {

Parameter::Parameter(std::string id, std::string name, ParamKind kind,
                     float minValue, float maxValue, float defaultValue)
    : id_(std::move(id)),
      name_(std::move(name)),
      kind_(kind),
      min_(minValue),
      max_(maxValue),
      default_(clampValue(defaultValue, minValue, maxValue)) {
    if (max_ <= min_) max_ = min_ + 1.0f;
    if (kind_ == ParamKind::Bool || kind_ == ParamKind::Choice || kind_ == ParamKind::Int)
        blend_ = ParamBlend::Stepped;
    value_.store(default_, std::memory_order_relaxed);
}

Parameter& Parameter::setChoices(std::vector<std::string> c) {
    choices_ = std::move(c);
    if (!choices_.empty()) {
        kind_ = ParamKind::Choice;
        blend_ = ParamBlend::Stepped;
        min_ = 0.0f;
        max_ = static_cast<float>(choices_.size() - 1);
        if (default_ > max_) default_ = max_;
        setValue(value());
    }
    return *this;
}

Parameter& Parameter::setSkewForCentre(float centre) {
    centre = clampValue(centre, min_, max_);
    const float proportion = (centre - min_) / (max_ - min_);
    if (proportion > 0.0f && proportion < 1.0f) {
        skew_ = std::log(0.5f) / std::log(proportion);
        curve_ = ParamCurve::Skewed;
    }
    return *this;
}

float Parameter::snap(float v) const noexcept {
    switch (kind_) {
        case ParamKind::Bool:   return v >= 0.5f ? 1.0f : 0.0f;
        case ParamKind::Choice:
        case ParamKind::Int:    return std::round(v);
        case ParamKind::Float:  break;
    }
    return v;
}

float Parameter::toNormalised(float v) const noexcept {
    v = clampValue(v, min_, max_);
    const float p = (v - min_) / (max_ - min_);
    if (curve_ == ParamCurve::Skewed && skew_ != 1.0f && p > 0.0f)
        return std::pow(p, skew_);
    return p;
}

float Parameter::fromNormalised(float n) const noexcept {
    n = clampValue(n, 0.0f, 1.0f);
    float p = n;
    if (curve_ == ParamCurve::Skewed && skew_ != 1.0f && n > 0.0f)
        p = std::pow(n, 1.0f / skew_);
    return snap(min_ + p * (max_ - min_));
}

std::string Parameter::toText(float v) const {
    char buf[64];

    switch (kind_) {
        case ParamKind::Bool:
            return v >= 0.5f ? "on" : "off";

        case ParamKind::Choice: {
            const int i = static_cast<int>(std::round(v));
            if (i >= 0 && i < static_cast<int>(choices_.size()))
                return choices_[static_cast<std::size_t>(i)];
            return "-";
        }

        case ParamKind::Int:
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::round(v)));
            break;

        case ParamKind::Float: {
            if (curve_ == ParamCurve::Decibels) {
                // -inf is more informative than "-96.0 dB" at the bottom of a fader.
                if (v <= min_ + 0.01f && min_ <= -90.0f)
                    return "-inf dB";
                std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
            } else {
                const float span = max_ - min_;
                const int decimals = span >= 1000.0f ? 0 : (span >= 100.0f ? 1 : (span >= 1.0f ? 2 : 3));
                std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
            }
            break;
        }
    }

    std::string s(buf);
    if (!unit_.empty()) { s += ' '; s += unit_; }
    return s;
}

bool Parameter::fromText(const std::string& text, float& out) const {
    // Trim.
    std::size_t b = 0, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    const std::string t = text.substr(b, e - b);
    if (t.empty()) return false;

    if (kind_ == ParamKind::Bool) {
        std::string lower;
        for (char c : t) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower == "on" || lower == "true" || lower == "yes" || lower == "1") { out = 1.0f; return true; }
        if (lower == "off" || lower == "false" || lower == "no" || lower == "0") { out = 0.0f; return true; }
        return false;
    }

    if (kind_ == ParamKind::Choice) {
        for (std::size_t i = 0; i < choices_.size(); ++i) {
            if (choices_[i].size() != t.size()) continue;
            bool match = true;
            for (std::size_t k = 0; k < t.size(); ++k) {
                if (std::tolower(static_cast<unsigned char>(choices_[i][k]))
                    != std::tolower(static_cast<unsigned char>(t[k]))) { match = false; break; }
            }
            if (match) { out = static_cast<float>(i); return true; }
        }
        // Fall through: allow typing the raw index.
    }

    if (t == "-inf" || t == "-INF" || t == "-inf dB") { out = min_; return true; }

    char* end = nullptr;
    const double parsed = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return false;

    out = snap(clampValue(static_cast<float>(parsed), min_, max_));
    return true;
}

} // namespace acm

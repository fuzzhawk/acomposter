#include "Fft.h"

#include <algorithm>

namespace acm::dsp {
namespace {

constexpr double kPi = 3.14159265358979323846;

int roundUpToPowerOfTwo(int value) {
    int size = 2;
    while (size < value) size <<= 1;
    return size;
}

} // namespace

void Fft::resize(int size) {
    size_ = roundUpToPowerOfTwo(std::max(2, size));

    levels_ = 0;
    for (int n = size_; n > 1; n >>= 1) ++levels_;

    cosTable_.resize(static_cast<std::size_t>(size_ / 2));
    sinTable_.resize(static_cast<std::size_t>(size_ / 2));

    for (int i = 0; i < size_ / 2; ++i) {
        const double angle = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(size_);
        cosTable_[static_cast<std::size_t>(i)] = static_cast<float>(std::cos(angle));
        sinTable_[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(angle));
    }

    real_.assign(static_cast<std::size_t>(size_), 0.0f);
    imag_.assign(static_cast<std::size_t>(size_), 0.0f);
}

void Fft::forward(float* real, float* imag) const {
    const int n = size_;

    // Bit-reversal permutation.
    for (int i = 0; i < n; ++i) {
        int reversed = 0;
        for (int bit = 0, value = i; bit < levels_; ++bit, value >>= 1)
            reversed = (reversed << 1) | (value & 1);

        if (reversed > i) {
            std::swap(real[i], real[reversed]);
            std::swap(imag[i], imag[reversed]);
        }
    }

    for (int span = 2; span <= n; span <<= 1) {
        const int half = span / 2;
        const int step = n / span;

        for (int i = 0; i < n; i += span) {
            for (int j = i, k = 0; j < i + half; ++j, k += step) {
                const int l = j + half;
                const float c = cosTable_[static_cast<std::size_t>(k)];
                const float s = sinTable_[static_cast<std::size_t>(k)];

                const float tr = real[l] * c + imag[l] * s;
                const float ti = -real[l] * s + imag[l] * c;

                real[l] = real[j] - tr;
                imag[l] = imag[j] - ti;
                real[j] += tr;
                imag[j] += ti;
            }
        }

        if (span == n) break;   // guards the shift overflowing on the last pass
    }
}

void Fft::inverse(float* real, float* imag) const {
    // The conjugate trick: swapping the parts, running the forward transform and
    // swapping back is the inverse up to a scale factor.
    forward(imag, real);

    const float scale = 1.0f / static_cast<float>(size_);
    for (int i = 0; i < size_; ++i) {
        real[i] *= scale;
        imag[i] *= scale;
    }
}

void Fft::magnitude(const float* samples, int count, std::vector<float>& outMagnitude,
                    bool window) const {
    const int n = size_;
    const int usable = std::min(count, n);

    for (int i = 0; i < usable; ++i) {
        float value = samples[i];
        if (window) {
            const double phase = 2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1);
            value *= static_cast<float>(0.5 - 0.5 * std::cos(phase));
        }
        real_[static_cast<std::size_t>(i)] = value;
    }
    std::fill(real_.begin() + usable, real_.end(), 0.0f);
    std::fill(imag_.begin(), imag_.end(), 0.0f);

    forward(real_.data(), imag_.data());

    outMagnitude.resize(static_cast<std::size_t>(n / 2));
    for (int i = 0; i < n / 2; ++i) {
        const float re = real_[static_cast<std::size_t>(i)];
        const float im = imag_[static_cast<std::size_t>(i)];
        outMagnitude[static_cast<std::size_t>(i)] = std::sqrt(re * re + im * im);
    }
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------

bool frequencyToNote(double hertz, int& outSemitonesFromA4, double& outCents) noexcept {
    if (hertz < 16.0 || hertz > 20000.0) return false;

    const double semitones = 12.0 * std::log2(hertz / 440.0);
    const double nearest = std::round(semitones);

    outSemitonesFromA4 = static_cast<int>(nearest);
    outCents = (semitones - nearest) * 100.0;
    return true;
}

const char* noteNameForSemitone(int semitonesFromA4) noexcept {
    static const char* kNames[12] = { "A", "A#", "B", "C", "C#", "D",
                                      "D#", "E", "F", "F#", "G", "G#" };
    // C++ truncates toward zero, so a negative index needs the extra wrap or
    // everything below A4 comes out one name off.
    int index = semitonesFromA4 % 12;
    if (index < 0) index += 12;
    return kNames[index];
}

int octaveForSemitone(int semitonesFromA4) noexcept {
    // A4 is 440 Hz and sits in octave 4; C is where the octave number steps, and
    // C4 is three semitones below A4.
    const int fromC0 = semitonesFromA4 + 57;   // A4 is 57 semitones above C0
    return fromC0 >= 0 ? fromC0 / 12 : (fromC0 - 11) / 12;
}

} // namespace acm::dsp

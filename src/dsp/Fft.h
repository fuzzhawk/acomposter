// A radix-2 FFT, and the magnitude spectrum built on it.
//
// Small on purpose. Everything here analyses recorded audio on the message
// thread - spectrograms, spectral features, pitch detection - where a few
// milliseconds either way is invisible. Nothing in the render path uses it, so
// there is no case for a split-radix implementation or a plan cache; a plain
// iterative Cooley-Tukey with a precomputed twiddle table is easier to read and
// fast enough by an order of magnitude.
//
// Sizes are powers of two only. A caller with an awkward length zero-pads up,
// which is what any windowed analysis wants anyway.
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace acm::dsp {

class Fft {
public:
    // `size` is rounded up to the next power of two, minimum 2.
    explicit Fft(int size) { resize(size); }

    void resize(int size);
    int size() const noexcept { return size_; }

    // In-place, on interleaved-free split arrays. `real` and `imag` must both
    // hold size() entries; `imag` is zero for real input.
    void forward(float* real, float* imag) const;
    void inverse(float* real, float* imag) const;

    // The convenience the analysis actually uses: window a block of real
    // samples, transform it, and fill `outMagnitude` with size()/2 bins.
    //
    // A Hann window is applied unless `window` is false. Without one, a tone
    // that does not sit exactly on a bin centre smears across the whole
    // spectrum and every feature computed from it is wrong.
    void magnitude(const float* samples, int count, std::vector<float>& outMagnitude,
                   bool window = true) const;

    // Frequency at the centre of bin `index`, in Hz.
    double binFrequency(int index, double sampleRate) const noexcept {
        return static_cast<double>(index) * sampleRate / static_cast<double>(size_);
    }

private:
    int size_ = 0;
    int levels_ = 0;
    std::vector<float> cosTable_;   // size/2
    std::vector<float> sinTable_;
    // Scratch for magnitude(), so repeated calls do not allocate.
    mutable std::vector<float> real_;
    mutable std::vector<float> imag_;
};

// The note a frequency is nearest, as a semitone index from A4 = 0, plus how
// far off it is in cents. Returns false for frequencies outside hearing.
bool frequencyToNote(double hertz, int& outSemitonesFromA4, double& outCents) noexcept;

// "A#3" for the semitone index frequencyToNote returns.
const char* noteNameForSemitone(int semitonesFromA4) noexcept;
int octaveForSemitone(int semitonesFromA4) noexcept;

} // namespace acm::dsp

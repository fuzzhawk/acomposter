#include "AudioAnalysis.h"

#include "../dsp/Fft.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace acm::library {
namespace {

// The band edges, in Hz. Logarithmic because hearing is: the distance from 40
// to 80 Hz matters as much as the one from 8 to 16 kHz.
constexpr double kBandEdges[kSpectrumBands + 1] = {
    40.0, 90.0, 200.0, 450.0, 1000.0, 2200.0, 5000.0, 10000.0, 16000.0
};

// Mixes to mono into `out`. Everything below works on one channel: a stereo
// file's two sides are the same sound, and analysing them separately would
// double the cost to produce two nearly identical answers.
void mixToMono(const SampleBuffer& buffer, std::vector<float>& out) {
    const auto frames = static_cast<std::size_t>(buffer.frames());
    out.assign(frames, 0.0f);
    if (buffer.channels() <= 0) return;

    for (int c = 0; c < buffer.channels(); ++c) {
        const float* data = buffer.channel(c);
        for (std::size_t i = 0; i < frames; ++i) out[i] += data[i];
    }

    const float scale = 1.0f / static_cast<float>(buffer.channels());
    for (float& sample : out) sample *= scale;
}

// The strongest pitch, by autocorrelation over the loudest part of the file.
//
// Autocorrelation rather than a spectral peak because the loudest partial is
// often not the fundamental - a bass note through a small speaker chain has
// almost no energy at its root - and octave errors are the one mistake a
// harmonic search cannot survive.
void detectPitch(const std::vector<float>& mono, double sampleRate,
                 double& outHertz, float& outConfidence) {
    outHertz = 0.0;
    outConfidence = 0.0f;

    if (sampleRate <= 0.0 || mono.size() < 2048) return;

    // Analyse from where the sound actually starts, over up to half a second.
    std::size_t start = 0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < mono.size(); ++i) peak = std::max(peak, std::fabs(mono[i]));
    if (peak < 1.0e-5f) return;

    for (std::size_t i = 0; i < mono.size(); ++i) {
        if (std::fabs(mono[i]) > peak * 0.25f) { start = i; break; }
    }

    // A quarter second is plenty to hear a pitch and keeps a folder scan of
    // thousands of files to something a person will wait for.
    const auto window = std::min(mono.size() - start,
                                 static_cast<std::size_t>(sampleRate * 0.25));
    if (window < 1024) return;

    const float* data = mono.data() + start;

    // Lags covering 40 Hz to 2 kHz. Below that is rumble and above it is noise
    // as far as "what note is this" is concerned.
    const auto minLag = static_cast<std::size_t>(sampleRate / 2000.0);
    const auto maxLag = std::min(static_cast<std::size_t>(sampleRate / 40.0), window / 2);
    if (maxLag <= minLag + 1) return;

    // Prefix sums of x^2, so the energy of any span is a subtraction. The
    // normalised score below needs the energy of both halves of every lag, and
    // recomputing those turns an O(n) inner loop into O(n) done three times.
    std::vector<double> energyUpTo(window + 1, 0.0);
    for (std::size_t i = 0; i < window; ++i)
        energyUpTo[i + 1] = energyUpTo[i] + static_cast<double>(data[i]) * data[i];

    if (energyUpTo[window] < 1.0e-9) return;

    // The true correlation coefficient, not the raw product. Dividing by the
    // energy of the two overlapping spans bounds it at 1 and - the point of the
    // exercise - collapses it toward zero for noise, where the raw product
    // stayed high enough to have white noise reported as a D#5.
    const auto scoreAt = [&](std::size_t lag) -> double {
        if (lag < minLag || lag > maxLag) return 0.0;

        const std::size_t overlap = window - lag;
        double sum = 0.0;
        for (std::size_t i = 0; i < overlap; ++i)
            sum += static_cast<double>(data[i]) * data[i + lag];

        const double left = energyUpTo[overlap] - energyUpTo[0];
        const double right = energyUpTo[window] - energyUpTo[lag];
        const double denominator = std::sqrt(left * right);
        return denominator > 1.0e-12 ? sum / denominator : 0.0;
    };

    double bestScore = 0.0;
    std::size_t bestLag = 0;

    for (std::size_t lag = minLag; lag <= maxLag; ++lag) {
        const double score = scoreAt(lag);
        if (score > bestScore) { bestScore = score; bestLag = lag; }
    }

    if (bestLag == 0) return;

    // Octave correction, and it is not optional. A periodic signal correlates
    // just as well with itself two, three or four periods along as it does at
    // one, so the highest score is as likely to be a sub-multiple of the true
    // pitch as the pitch itself - a 110 Hz tone came out as 55 Hz, an octave
    // down, every time. Any shorter lag that scores nearly as well is the
    // better answer, because a real octave-down partial would not.
    for (int divisor = 8; divisor >= 2; --divisor) {
        const std::size_t candidate = bestLag / static_cast<std::size_t>(divisor);
        if (candidate < minLag) continue;
        if (scoreAt(candidate) > bestScore * 0.85) {
            bestLag = candidate;
            bestScore = scoreAt(candidate);
            break;
        }
    }

    const double left = scoreAt(bestLag - 1);
    const double right = scoreAt(bestLag + 1);
    const double denominator = left - 2.0 * bestScore + right;
    double refined = static_cast<double>(bestLag);
    if (std::fabs(denominator) > 1.0e-12) refined += 0.5 * (left - right) / denominator;

    outHertz = sampleRate / std::max(1.0, refined);

    // Confidence is that coefficient: a sine hits nearly 1, a cymbal stays low.
    outConfidence = static_cast<float>(std::clamp(bestScore, 0.0, 1.0));
}

} // namespace

std::vector<int> extractNumbers(const std::string& text) {
    std::vector<int> numbers;

    for (std::size_t i = 0; i < text.size();) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; continue; }

        std::size_t end = i;
        while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;

        // Anything longer than nine digits is a hash or a timestamp rather than
        // a take number, and would overflow an int on the way in.
        if (end - i <= 9) numbers.push_back(std::stoi(text.substr(i, end - i)));
        i = end;
    }

    return numbers;
}

Analysis analyse(const SampleBuffer& buffer, const std::string& utf8Name) {
    Analysis analysis;
    if (buffer.empty()) return analysis;

    analysis.valid = true;
    analysis.sampleRate = buffer.sampleRate();
    analysis.channels = buffer.channels();
    analysis.durationSeconds = buffer.durationSeconds();
    analysis.bpm = buffer.detectedBpm;
    analysis.filenameNumbers = extractNumbers(utf8Name);

    std::vector<float> mono;
    mixToMono(buffer, mono);
    if (mono.empty()) return analysis;

    // -- level -------------------------------------------------------------
    double squared = 0.0;
    for (const float sample : mono) {
        analysis.peak = std::max(analysis.peak, std::fabs(sample));
        squared += static_cast<double>(sample) * sample;
    }
    analysis.rms = static_cast<float>(std::sqrt(squared / static_cast<double>(mono.size())));

    // -- envelope ----------------------------------------------------------
    // Attack is time to the peak; decay is time from the peak back to a tenth
    // of it. Both measured on the raw envelope, which is crude and legible -
    // the numbers only ever get compared to each other.
    if (analysis.peak > 1.0e-5f) {
        std::size_t peakIndex = 0;
        for (std::size_t i = 0; i < mono.size(); ++i) {
            if (std::fabs(mono[i]) >= analysis.peak) { peakIndex = i; break; }
        }
        analysis.attackSeconds = static_cast<double>(peakIndex) / analysis.sampleRate;

        const float threshold = analysis.peak * 0.1f;
        std::size_t decayIndex = mono.size() - 1;
        for (std::size_t i = mono.size(); i-- > peakIndex;) {
            if (std::fabs(mono[i]) > threshold) { decayIndex = i; break; }
        }
        analysis.decaySeconds =
            static_cast<double>(decayIndex - peakIndex) / analysis.sampleRate;
    }

    // -- spectrum ----------------------------------------------------------
    // Averaged over several windows across the file rather than one at the
    // start: a sound whose first 20 ms is a click would otherwise be filed as
    // a click.
    constexpr int kFftSize = 2048;
    dsp::Fft fft(kFftSize);

    std::vector<float> magnitude;
    std::vector<double> bandSums(kSpectrumBands, 0.0);
    double centroidNumerator = 0.0;
    double centroidDenominator = 0.0;
    int windows = 0;

    const auto step = std::max<std::size_t>(kFftSize / 2, 1);
    for (std::size_t offset = 0; offset + kFftSize <= mono.size() && windows < 64;
         offset += step, ++windows) {
        fft.magnitude(mono.data() + offset, kFftSize, magnitude);

        for (int bin = 1; bin < static_cast<int>(magnitude.size()); ++bin) {
            const double hz = fft.binFrequency(bin, analysis.sampleRate);
            const double value = magnitude[static_cast<std::size_t>(bin)];

            centroidNumerator += hz * value;
            centroidDenominator += value;

            for (int band = 0; band < kSpectrumBands; ++band) {
                if (hz >= kBandEdges[band] && hz < kBandEdges[band + 1]) {
                    bandSums[static_cast<std::size_t>(band)] += value;
                    break;
                }
            }
        }
    }

    // A file shorter than one window still gets a spectrum, from what there is.
    if (windows == 0 && mono.size() >= 64) {
        fft.magnitude(mono.data(), static_cast<int>(mono.size()), magnitude);
        for (int bin = 1; bin < static_cast<int>(magnitude.size()); ++bin) {
            const double hz = fft.binFrequency(bin, analysis.sampleRate);
            const double value = magnitude[static_cast<std::size_t>(bin)];
            centroidNumerator += hz * value;
            centroidDenominator += value;
            for (int band = 0; band < kSpectrumBands; ++band) {
                if (hz >= kBandEdges[band] && hz < kBandEdges[band + 1]) {
                    bandSums[static_cast<std::size_t>(band)] += value;
                    break;
                }
            }
        }
    }

    if (centroidDenominator > 1.0e-12) analysis.centroidHz = centroidNumerator / centroidDenominator;

    double bandTotal = 0.0;
    for (const double sum : bandSums) bandTotal += sum;
    if (bandTotal > 1.0e-12) {
        for (int band = 0; band < kSpectrumBands; ++band)
            analysis.bands[band] = static_cast<float>(bandSums[static_cast<std::size_t>(band)]
                                                      / bandTotal);
    }

    // -- pitch -------------------------------------------------------------
    detectPitch(mono, analysis.sampleRate, analysis.pitchHz, analysis.pitchConfidence);

    // A fundamental cannot sit far above the spectrum's centre of mass: the
    // centroid of harmonic content is at or above the root, never well below
    // it. Without this check a kick - a sine swept down over 20 ms, which has
    // no stable period for the autocorrelation to find - came back as a
    // confident B6, two octaves above anything actually in the file.
    const bool plausible = analysis.centroidHz > 20.0
                         && analysis.pitchHz < analysis.centroidHz * 2.0;

    if (analysis.pitchConfidence > 0.35f && plausible) {
        double cents = 0.0;
        if (dsp::frequencyToNote(analysis.pitchHz, analysis.semitonesFromA4, cents)) {
            char name[16];
            std::snprintf(name, sizeof(name), "%s%d",
                          dsp::noteNameForSemitone(analysis.semitonesFromA4),
                          dsp::octaveForSemitone(analysis.semitonesFromA4));
            analysis.noteName = name;
        }
    }

    return analysis;
}

float similarity(const Analysis& a, const Analysis& b) noexcept {
    if (!a.valid || !b.valid) return 0.0f;

    // Band shape: the sum of absolute differences, which for two distributions
    // that each sum to 1 lands in 0..2.
    double bandDistance = 0.0;
    for (int i = 0; i < kSpectrumBands; ++i)
        bandDistance += std::fabs(static_cast<double>(a.bands[i]) - b.bands[i]);
    const double bandScore = 1.0 - std::min(1.0, bandDistance / 2.0);

    // Brightness, compared in octaves so 200 Hz against 400 counts the same as
    // 4 kHz against 8.
    double centroidScore = 0.5;
    if (a.centroidHz > 20.0 && b.centroidHz > 20.0) {
        const double octaves = std::fabs(std::log2(a.centroidHz / b.centroidHz));
        centroidScore = std::max(0.0, 1.0 - octaves / 4.0);
    }

    // Length, also logarithmic: a half-second hit is much more like a one
    // second hit than a four second one is like an eight.
    double lengthScore = 0.5;
    if (a.durationSeconds > 0.01 && b.durationSeconds > 0.01) {
        const double ratio = std::fabs(std::log2(a.durationSeconds / b.durationSeconds));
        lengthScore = std::max(0.0, 1.0 - ratio / 4.0);
    }

    double total = bandScore * 0.55 + centroidScore * 0.25 + lengthScore * 0.10;
    double weight = 0.90;

    // Pitch only counts when both are actually pitched. Comparing the detected
    // pitch of two cymbals makes them similar or different at random, which is
    // worse than not comparing them at all.
    if (a.pitchConfidence > 0.35f && b.pitchConfidence > 0.35f) {
        const int difference = std::abs(a.semitonesFromA4 - b.semitonesFromA4) % 12;
        const int distance = std::min(difference, 12 - difference);
        total += (1.0 - static_cast<double>(distance) / 6.0) * 0.10;
        weight += 0.10;
    }

    return static_cast<float>(std::clamp(total / weight, 0.0, 1.0));
}

bool sameePitchClass(int semitonesA, int semitonesB) noexcept {
    int a = semitonesA % 12;
    int b = semitonesB % 12;
    if (a < 0) a += 12;
    if (b < 0) b += 12;
    return a == b;
}

Spectrogram computeSpectrogram(const SampleBuffer& buffer, int fftSize, int maxFrames) {
    Spectrogram out;
    if (buffer.empty() || maxFrames <= 0) return out;

    std::vector<float> mono;
    mixToMono(buffer, mono);
    if (mono.size() < static_cast<std::size_t>(fftSize)) return out;

    dsp::Fft fft(fftSize);
    const int bins = fft.size() / 2;

    // Hop chosen so the whole file fits in maxFrames columns. The view stretches
    // the result to its width, so a short file gets more detail per column and a
    // long one less, which is the right way round.
    const auto span = mono.size() - static_cast<std::size_t>(fft.size());
    const auto hop = std::max<std::size_t>(1, span / static_cast<std::size_t>(maxFrames));

    std::vector<float> magnitude;
    float loudest = 0.0f;

    for (std::size_t offset = 0;
         offset + static_cast<std::size_t>(fft.size()) <= mono.size()
             && out.frames < maxFrames;
         offset += hop, ++out.frames) {
        fft.magnitude(mono.data() + offset, fft.size(), magnitude);
        for (int bin = 0; bin < bins; ++bin) {
            const float value = magnitude[static_cast<std::size_t>(bin)];
            loudest = std::max(loudest, value);
            out.magnitudes.push_back(value);
        }
    }

    out.bins = bins;
    out.sampleRate = buffer.sampleRate();

    // Normalised in dB rather than linearly: a linear spectrogram of anything
    // real is one bright line at the bottom and black everywhere else.
    if (loudest > 1.0e-9f) {
        constexpr float kFloorDb = -80.0f;
        for (float& value : out.magnitudes) {
            const float db = 20.0f * std::log10(std::max(value / loudest, 1.0e-6f));
            value = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
        }
    }

    return out;
}

} // namespace acm::library

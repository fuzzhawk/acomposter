// What a file sounds like, reduced to numbers a librarian can sort and search.
//
// The point of this is finding things. A folder of ten thousand one-shots is
// unusable by name alone - names are inconsistent, often wrong, and say nothing
// about what the sound actually is. So each file gets analysed once into a
// small fixed set of features, and those features answer the questions that
// matter when building a track: what key is it in, is it bright or dull, is it
// a hit or a texture, and what else in here sounds like it.
//
// The features are deliberately few and cheap. A file's analysis has to be
// storable, comparable and explainable - a similarity score nobody can account
// for is one nobody trusts - and forty dimensions of MFCCs are none of those.
#pragma once

#include "../audio/SampleBuffer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace acm::library {

// Eight logarithmically spaced bands from 40 Hz to 16 kHz. Enough to tell a
// kick from a snare from a pad, few enough to read as a bar chart.
inline constexpr int kSpectrumBands = 8;

struct Analysis {
    bool valid = false;

    double durationSeconds = 0.0;
    double sampleRate = 0.0;
    int channels = 0;

    float peak = 0.0f;          // linear
    float rms = 0.0f;           // linear, over the sounding part

    // Normalised band energies, summing to 1. The shape of the sound.
    float bands[kSpectrumBands] = {};

    // Brightness, as the spectral centroid in Hz. The single most useful
    // number here: it separates a sub from a hat without any further thought.
    double centroidHz = 0.0;

    // The strongest pitch, and how confident that is. Percussive material has a
    // pitch too but a low confidence, which is exactly the distinction a
    // harmonic search needs.
    double pitchHz = 0.0;
    float pitchConfidence = 0.0f;
    int semitonesFromA4 = 0;
    std::string noteName;       // "C#3", empty when unpitched

    // How fast it starts and how long it takes to die: a hit and a swell have
    // the same spectrum and are not interchangeable.
    double attackSeconds = 0.0;
    double decaySeconds = 0.0;

    // Tempo taken from the file name or an ACID chunk, 0 when unknown.
    double bpm = 0.0;

    // Numbers found in the file name, in the order they appear. A folder named
    // by hand is full of them - "kick 03", "loop_174", "take 2" - and being
    // able to sort or filter on them is most of what a rename would have been
    // for.
    std::vector<int> filenameNumbers;
};

// Analyses a loaded buffer. `utf8Name` is used for the filename numbers and for
// the tempo hint; pass the leaf name, not the whole path.
Analysis analyse(const SampleBuffer& buffer, const std::string& utf8Name);

// Every run of digits in a name, as integers. "kick_03_174bpm" -> {3, 174}.
// Runs longer than nine digits are skipped rather than overflowing.
std::vector<int> extractNumbers(const std::string& text);

// How alike two analyses are, from 0 (nothing in common) to 1 (identical).
//
// Weighted toward what the ear notices first: the band shape dominates,
// brightness and length matter, and pitch only counts when both sides are
// actually pitched. Comparing the pitch of two cymbals would otherwise make
// them similar or different at random.
float similarity(const Analysis& a, const Analysis& b) noexcept;

// True when two notes belong to the same key, ignoring octave. Used by the
// harmonic search, which has to match a C2 bass to a C4 pad.
bool sameePitchClass(int semitonesA, int semitonesB) noexcept;

// A spectrogram for the 3D view: `frames` columns of `bins` magnitudes each,
// normalised to 0..1. Only worth doing for short files, which is why the view
// that uses it refuses anything longer.
struct Spectrogram {
    int frames = 0;
    int bins = 0;
    double sampleRate = 0.0;
    std::vector<float> magnitudes;   // frames * bins, row-major by frame

    float at(int frame, int bin) const noexcept {
        if (frame < 0 || frame >= frames || bin < 0 || bin >= bins) return 0.0f;
        return magnitudes[static_cast<std::size_t>(frame) * static_cast<std::size_t>(bins)
                          + static_cast<std::size_t>(bin)];
    }
};

Spectrogram computeSpectrogram(const SampleBuffer& buffer, int fftSize = 512,
                               int maxFrames = 160);

} // namespace acm::library

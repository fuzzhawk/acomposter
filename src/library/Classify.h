// Guessing what an audio file is, from what the analysis measured.
//
// This exists to make a folder of badly-named files tractable: proposing
// "kick", "snare", "pad" for a few thousand samples and letting a person
// correct the wrong ones is an afternoon, and naming them by hand is not.
//
// Every guess carries a confidence and a reason. The reason is not decoration.
// The whole thing is a set of thresholds on eight band energies, a centroid and
// an envelope - it is going to be wrong sometimes, and a person approving a
// hundred guesses needs to see *why* each one was made in order to spot the
// ones that are wrong at a glance rather than by auditioning all of them.
//
// One thing deliberately not guessed: vocals. Nothing in these features
// separates a sung note from a synth lead - both are pitched, both sit in the
// same band, both sustain - and a classifier that confidently mislabels half a
// vocal folder as leads is worse than one that says it does not know.
#pragma once

#include "AudioAnalysis.h"

#include <string>

namespace acm::library {

enum class Instrument : int {
    Unknown = 0,
    Kick,
    Snare,
    HiHat,
    Percussion,
    Bass,
    Lead,
    Pad,
    Fx,
    Count
};

const char* toString(Instrument instrument) noexcept;

struct Classification {
    Instrument instrument = Instrument::Unknown;
    // 0..1. Below about 0.5 the guess is a suggestion rather than an answer,
    // and the wizard shows it differently.
    float confidence = 0.0f;
    // Why, in the terms the decision was actually made in: "short, low-heavy,
    // fast attack". Shown next to the guess so it can be checked by eye.
    std::string reason;
};

Classification classify(const Analysis& analysis);

// The tag in `palette` that best matches an instrument, by name rather than by
// id: a palette is user-editable and renameable, so matching ids would break
// the moment anyone customised theirs. Returns an empty string when the palette
// has nothing suitable.
class TagPalette;
std::string tagForInstrument(const TagPalette& palette, Instrument instrument);

// A name proposed for a file, built from what it is and what its old name knew.
// "kick_03.wav" classified as a kick keeps its 03; a file whose name says
// nothing gets the index it was handed.
std::string proposeName(const Analysis& analysis, Instrument instrument, int index);

} // namespace acm::library

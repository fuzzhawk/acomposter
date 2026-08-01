// Cutting one long file into the hits it is made of.
//
// A folder of drum loops and a folder of drum hits are different tools, and
// turning the first into the second by hand is an hour per loop. The slicer
// finds where the transients are, and the wizard writes each span out as its
// own file - which then goes through the same analysis and classification as
// anything else, so a sliced loop arrives already tagged.
//
// Detection is by spectral flux: the rise in energy from one short window to
// the next, which finds a hit landing on top of a decaying one where plain
// amplitude does not. That is the case that matters, because it is every
// snare over a ringing kick.
#pragma once

#include "../audio/SampleBuffer.h"

#include <cstdint>
#include <vector>

namespace acm::library {

struct SliceSettings {
    // How far above the running average a flux peak has to sit to count. Higher
    // finds only the obvious hits; lower finds ghost notes and, eventually,
    // noise.
    float sensitivity = 1.5f;

    // Nothing closer together than this. Without it a single hit's attack
    // produces three slices a millisecond apart.
    double minimumGapSeconds = 0.05;

    // Slices shorter than this are dropped rather than written as files nobody
    // wants.
    double minimumLengthSeconds = 0.02;
};

// Frame indices where slices start. Always begins at the first sounding frame,
// so the head of the file is a slice rather than being discarded.
std::vector<std::int64_t> findSlicePoints(const SampleBuffer& buffer,
                                          const SliceSettings& settings = {});

// One slice, copied out. `end` is exclusive; passing the buffer's length for
// the last slice is the normal case.
std::shared_ptr<SampleBuffer> extractSlice(const SampleBuffer& buffer,
                                           std::int64_t start, std::int64_t end);

} // namespace acm::library

// The colour presets that ship.
//
// A colour preset says, for each parameter it drives, where that parameter sits
// at the red end and at the blue end. It binds by *parameter name*, so a preset
// written against one filter works against a different one that calls its
// controls the same thing - and reports what it could not find rather than
// binding to the wrong control.
//
// That name matching is why these are worth shipping at all. There is no stable
// parameter map for any commercial plugin: FabFilter Pro-Q numbers its bands in
// the order they were created, Volcano's modulation slots move with the
// routing, and Bloom's matrix is built by the user. Nothing can be addressed by
// index. But almost every filter has a control called "Frequency" or "Cutoff",
// almost every reverb has "Mix" or "Wet", and a preset built on those names
// binds to a good fraction of whatever is actually on the rack.
//
// So these are a starting point rather than an answer: they will bind
// partially, say so, and leave the ends to be captured by ear - which is what
// the capture buttons are for.
#pragma once

#include "PresetStore.h"

namespace acm::library {

// Writes the shipped presets into `store`, skipping any that already exist so
// an edited one is never overwritten. Called once when a library opens.
void seedColourPresets(const PresetStore& store);

} // namespace acm::library

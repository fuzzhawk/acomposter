#include "Classify.h"

#include "TagPalette.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace acm::library {
namespace {

// The eight bands, grouped into the four ranges the ear actually reasons in.
struct Ranges {
    float low = 0.0f;      // 40 - 200 Hz
    float lowMid = 0.0f;   // 200 - 1000
    float mid = 0.0f;      // 1k - 5k
    float high = 0.0f;     // 5k - 16k
};

Ranges rangesOf(const Analysis& analysis) {
    Ranges r;
    r.low = analysis.bands[0] + analysis.bands[1];
    r.lowMid = analysis.bands[2] + analysis.bands[3];
    r.mid = analysis.bands[4] + analysis.bands[5];
    r.high = analysis.bands[6] + analysis.bands[7];
    return r;
}

std::string lowerCase(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

const char* toString(Instrument instrument) noexcept {
    switch (instrument) {
        case Instrument::Kick:       return "kick";
        case Instrument::Snare:      return "snare";
        case Instrument::HiHat:      return "hat";
        case Instrument::Percussion: return "perc";
        case Instrument::Bass:       return "bass";
        case Instrument::Lead:       return "lead";
        case Instrument::Pad:        return "pad";
        case Instrument::Fx:         return "fx";
        case Instrument::Unknown:
        default:                     return "unknown";
    }
}

Classification classify(const Analysis& analysis) {
    Classification out;
    if (!analysis.valid) return out;

    const Ranges r = rangesOf(analysis);
    const double seconds = analysis.durationSeconds;
    const bool pitched = !analysis.noteName.empty();
    const bool fastAttack = analysis.attackSeconds < 0.03;

    // -- percussive ---------------------------------------------------------
    // Order matters: a kick is also short and also has a fast attack, so the
    // most specific tests come first.
    // Under 0.6 s, not 1.5. A low sine that rings for a second is a bass note,
    // and a kick that rings for a second is not a kick - the duration is what
    // separates them, since their spectra are the same shape.
    if (seconds < 0.6 && r.low > 0.55f && r.high < 0.15f && fastAttack) {
        out.instrument = Instrument::Kick;
        out.confidence = std::min(1.0f, 0.55f + r.low * 0.45f);
        out.reason = "short, low-heavy, fast attack";
        return out;
    }

    // Under a quarter second, not 0.4. A bright hit that rings for 300 ms is
    // far more likely to be a snare than a hat, and at 0.4 the hat rule was
    // catching snares before the snare rule ever ran.
    if (seconds < 0.25 && r.high > 0.5f) {
        out.instrument = Instrument::HiHat;
        out.confidence = std::min(1.0f, 0.5f + r.high * 0.5f);
        out.reason = "very short and bright";
        return out;
    }

    // Body is low *and* low-mid: a snare's fundamental sits around 180-200 Hz,
    // which falls in the 90-200 band rather than the 200-1k one, so testing
    // only low-mid missed it on exactly the material the rule is for.
    if (seconds < 1.0 && fastAttack && (r.mid + r.high) > 0.35f
        && (r.low + r.lowMid) > 0.15f) {
        out.instrument = Instrument::Snare;
        out.confidence = 0.55f + std::min(0.35f, (r.mid + r.high) * 0.35f);
        out.reason = "short, broadband, body around 200-1k";
        return out;
    }

    // Not conditional on being unpitched, and ahead of the pitched rules. Toms,
    // rimshots and congas all have a pitch, and a 300 ms hit at 200 Hz was
    // being filed as a bass note because the bass rule saw it first.
    if (seconds < 0.6 && fastAttack) {
        out.instrument = Instrument::Percussion;
        out.confidence = 0.45f;
        out.reason = pitched ? "short, pitched, fast attack" : "short and unpitched";
        return out;
    }

    // -- pitched ------------------------------------------------------------
    // Sustained, not just low. A bass note rings; anything shorter than a third
    // of a second has already been caught as percussion above.
    if (pitched && analysis.pitchHz < 220.0 && seconds > 0.35) {
        out.instrument = Instrument::Bass;
        out.confidence = std::min(1.0f, 0.5f + analysis.pitchConfidence * 0.5f);
        char reason[96];
        std::snprintf(reason, sizeof(reason), "pitched at %s, below 220 Hz",
                      analysis.noteName.c_str());
        out.reason = reason;
        return out;
    }

    if (pitched && seconds >= 1.5 && !fastAttack) {
        out.instrument = Instrument::Pad;
        out.confidence = 0.55f;
        out.reason = "pitched, long, soft attack";
        return out;
    }

    if (pitched) {
        out.instrument = Instrument::Lead;
        out.confidence = std::min(1.0f, 0.45f + analysis.pitchConfidence * 0.4f);
        char reason[96];
        std::snprintf(reason, sizeof(reason), "pitched at %s", analysis.noteName.c_str());
        out.reason = reason;
        return out;
    }

    // -- everything else ----------------------------------------------------
    // Long and unpitched. Not a confident answer, and does not pretend to be:
    // it is the bucket for risers, impacts, noise beds and anything with a
    // shape these few numbers cannot describe.
    if (seconds >= 1.0) {
        out.instrument = Instrument::Fx;
        out.confidence = 0.35f;
        out.reason = "long and unpitched";
        return out;
    }

    out.reason = "nothing distinctive";
    return out;
}

std::string tagForInstrument(const TagPalette& palette, Instrument instrument) {
    // Matched by name rather than by id. A palette is renameable and
    // extensible, and matching on ids would break for anyone who edited theirs
    // - which is most of the point of it being editable.
    const char* aliases[static_cast<int>(Instrument::Count)][3] = {
        { nullptr, nullptr, nullptr },              // Unknown
        { "kick", "bass drum", "bd" },
        { "snare", "clap", "sd" },
        { "hat", "hats", "hi-hat" },
        { "percussion", "perc", "drums mixed" },
        { "bass", "sub", "808" },
        { "synth leads", "lead", "leads" },
        { "pads", "pad", "strings" },
        { "fx", "effects", "risers" },
    };

    const int index = static_cast<int>(instrument);
    if (index <= 0 || index >= static_cast<int>(Instrument::Count)) return {};

    for (const char* alias : aliases[index]) {
        if (!alias) continue;
        for (const Tag& tag : palette.tags())
            if (lowerCase(tag.name) == alias) return tag.id;
    }

    // Nothing exact. A partial match is better than nothing here: a palette
    // with "kick drum" on it should still catch a kick.
    for (const char* alias : aliases[index]) {
        if (!alias) continue;
        const std::string needle = alias;
        for (const Tag& tag : palette.tags()) {
            const std::string name = lowerCase(tag.name);
            if (name.find(needle) != std::string::npos) return tag.id;
        }
    }

    return {};
}

std::string proposeName(const Analysis& analysis, Instrument instrument, int index) {
    std::string out = toString(instrument);

    // A pitched sound is worth naming by its note: "bass-c2" beats "bass-07"
    // for anything that has to sit in a key.
    if (!analysis.noteName.empty()
        && (instrument == Instrument::Bass || instrument == Instrument::Lead
            || instrument == Instrument::Pad)) {
        out += "-" + lowerCase(analysis.noteName);
    }

    // The old name's *last* number is kept when there is one. A folder numbered
    // by hand carries information in those digits - takes, velocity layers,
    // round-robins - and renaming over them throws it away. The last rather
    // than the first because a name like "sub_a2_05" has a digit in its note
    // name, and the take number is the one at the end.
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%02d",
                  analysis.filenameNumbers.empty() ? index
                                                   : analysis.filenameNumbers.back());
    out += suffix;

    return out;
}

} // namespace acm::library

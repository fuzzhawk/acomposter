#include "StemPlayerNode.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"

#include <algorithm>
#include <cmath>

namespace acm {
namespace {

// Names the stem slots after what a bounce usually contains, so an empty node
// still says what it is for. Any of them can be renamed.
constexpr const char* kDefaultStemNames[kMaxStems] = {
    "drums", "bass", "music 1", "music 2", "music 3", "vocals", "fx", "other",
};

} // namespace

StemPlayerNode::StemPlayerNode() : Node("stem.player", NodeCategory::Source) {
    for (int i = 0; i < kMaxStems; ++i) {
        addOutput(kDefaultStemNames[i], 2);
        stems_[static_cast<std::size_t>(i)].name = kDefaultStemNames[i];
    }
    // A summed pair as well, so the node is useful before anything is patched
    // out of it and so a stem set can be auditioned without eight cables.
    addOutput("mix", 2);

    // -- performance -------------------------------------------------------
    // Stepped: the metasurface must snap to a section rather than average two,
    // and "halfway between the chorus and the breakdown" is not a place.
    pSection_ = indexOfParameter(
        addIntParam(kSectionParam, "Section", 0, kMaxSections - 1, 0)
            .setBlend(ParamBlend::Stepped).id());

    pLaunch_ = indexOfParameter(
        addChoiceParam("launch", "Launch",
                       { "end of loop", "next bar", "next beat", "immediate" }, 0)
            .setBlend(ParamBlend::Stepped).id());

    // Driven by the build-up generator: 1 is the whole section, 16 is a
    // sixteenth of it stuttering.
    pDivide_ = indexOfParameter(
        addChoiceParam(kDivideParam, "Loop Divide",
                       { "1", "2", "4", "8", "16", "32" }, 0)
            .setBlend(ParamBlend::Stepped).id());

    pRepeat_ = indexOfParameter(addBoolParam(kRepeatParam, "Repeat", false).id());

    pFollow_ = indexOfParameter(addBoolParam("follow", "Follow Transport", true).id());
    pGain_ = indexOfParameter(addDbParam("gain", "Gain", -60.0f, 12.0f, 0.0f).id());

    for (int i = 0; i < kMaxStems; ++i) {
        const std::string suffix = std::to_string(i + 1);
        pStemGain_[static_cast<std::size_t>(i)] = indexOfParameter(
            addDbParam("gain" + suffix, std::string(kDefaultStemNames[i]) + " gain",
                       -60.0f, 12.0f, 0.0f).id());
        pStemMute_[static_cast<std::size_t>(i)] = indexOfParameter(
            addBoolParam("mute" + suffix, std::string(kDefaultStemNames[i]) + " mute", false).id());
    }
}

// ---------------------------------------------------------------------------
// Stems
// ---------------------------------------------------------------------------

bool StemPlayerNode::loadStem(int slot, const std::string& utf8Path, std::string* error) {
    if (slot < 0 || slot >= kMaxStems) {
        if (error) *error = "no such stem slot";
        return false;
    }

    std::string loadError;
    // Stems are long - a whole song rather than a bar - so the ceiling is
    // raised accordingly. Overviews are built because the section editor draws
    // every stem's waveform against the bar grid.
    audiofile::LoadOptions options;
    options.maxDurationSeconds = 60.0 * 30.0;
    options.buildOverview = true;
    options.overviewBuckets = 4096;

    std::shared_ptr<SampleBuffer> buffer = audiofile::load(utf8Path, &loadError, options);
    if (!buffer) {
        if (error) *error = loadError;
        setErrorText(loadError);
        return false;
    }

    Stem& stem = stems_[static_cast<std::size_t>(slot)];
    buildSpectrum(*buffer, stem.spectrum, 512);
    stem.buffer.publish(std::move(buffer));
    stem.path = utf8Path;
    if (stem.name.empty() || stem.name == kDefaultStemNames[slot])
        stem.name = pathStem(utf8Path);

    setErrorText({});
    return true;
}

void StemPlayerNode::setStemFromBuffer(int slot, std::shared_ptr<SampleBuffer> buffer,
                                       std::string name) {
    if (slot < 0 || slot >= kMaxStems || !buffer) return;

    Stem& stem = stems_[static_cast<std::size_t>(slot)];
    buildSpectrum(*buffer, stem.spectrum, 512);
    stem.buffer.publish(std::move(buffer));
    stem.path.clear();
    if (!name.empty()) stem.name = std::move(name);
}

void StemPlayerNode::clearStem(int slot) {
    if (slot < 0 || slot >= kMaxStems) return;
    Stem& stem = stems_[static_cast<std::size_t>(slot)];
    stem.buffer.clear();
    stem.path.clear();
    stem.spectrum.clear();
    stem.name = kDefaultStemNames[slot];
}

std::shared_ptr<SampleBuffer> StemPlayerNode::stem(int slot) const {
    if (slot < 0 || slot >= kMaxStems) return nullptr;
    return stems_[static_cast<std::size_t>(slot)].buffer.shared();
}

const std::string& StemPlayerNode::stemPath(int slot) const {
    static const std::string empty;
    if (slot < 0 || slot >= kMaxStems) return empty;
    return stems_[static_cast<std::size_t>(slot)].path;
}

const std::string& StemPlayerNode::stemName(int slot) const {
    static const std::string empty;
    if (slot < 0 || slot >= kMaxStems) return empty;
    return stems_[static_cast<std::size_t>(slot)].name;
}

void StemPlayerNode::setStemName(int slot, std::string name) {
    if (slot < 0 || slot >= kMaxStems) return;
    stems_[static_cast<std::size_t>(slot)].name = std::move(name);
}

bool StemPlayerNode::stemLoaded(int slot) const {
    if (slot < 0 || slot >= kMaxStems) return false;
    const auto buffer = stems_[static_cast<std::size_t>(slot)].buffer.shared();
    return buffer && !buffer->empty();
}

double StemPlayerNode::songLengthBars(double bpm, int beatsPerBar) const {
    if (bpm <= 0.0 || beatsPerBar <= 0) return 0.0;

    double longestSeconds = 0.0;
    for (int i = 0; i < kMaxStems; ++i) {
        const auto buffer = stems_[static_cast<std::size_t>(i)].buffer.shared();
        if (buffer && !buffer->empty())
            longestSeconds = std::max(longestSeconds, buffer->durationSeconds());
    }

    const double beats = longestSeconds * bpm / 60.0;
    return beats / static_cast<double>(beatsPerBar);
}

float StemPlayerNode::stemPlayhead(int slot) const noexcept {
    if (slot < 0 || slot >= kMaxStems) return 0.0f;
    return playhead_[static_cast<std::size_t>(slot)].load(std::memory_order_relaxed);
}

float StemPlayerNode::meterLevel(int slot, int channel) const noexcept {
    if (slot < 0 || slot >= kMaxStems) return 0.0f;
    return stems_[static_cast<std::size_t>(slot)].meter[channel & 1]
        .load(std::memory_order_relaxed);
}


// ---------------------------------------------------------------------------
// Tempo
// ---------------------------------------------------------------------------

void StemPlayerNode::setStemBpm(double bpm) noexcept {
    stemBpm_ = bpm > 0.0 ? clampValue(bpm, 20.0, 400.0) : 0.0;
    tempoSource_ = stemBpm_ > 0.0 ? "set by hand" : "project";
}

double StemPlayerNode::detectBpm(int beatsPerBar, int* outBars) const {
    if (outBars) *outBars = 0;
    if (beatsPerBar <= 0) return 0.0;

    double longestSeconds = 0.0;
    double hint = 0.0;
    for (int i = 0; i < kMaxStems; ++i) {
        const Stem& stem = stems_[static_cast<std::size_t>(i)];
        const auto buffer = stem.buffer.shared();
        if (!buffer || buffer->empty()) continue;

        longestSeconds = std::max(longestSeconds, buffer->durationSeconds());
        // Exporters put the tempo in the file name far more often than in the
        // file, and it settles an otherwise unanswerable question.
        if (hint <= 0.0 && !stem.path.empty()) hint = audiofile::guessBpmFromName(stem.path);
        if (hint <= 0.0 && !stem.name.empty()) hint = audiofile::guessBpmFromName(stem.name);
    }
    if (longestSeconds < 1.0) return 0.0;

    // Length alone does not determine tempo. A thirty second bounce is *exactly*
    // a whole number of bars at eighteen different tempos between 60 and 200,
    // and no amount of arithmetic will tell you which one the music is in. So
    // this picks the most likely rather than the only: whole-bar candidates,
    // ranked by how ordinary the bar count and the tempo are, and pulled hard
    // toward the file name's tempo when there is one.
    double bestBpm = 0.0;
    double bestScore = 1.0e9;
    int bestBars = 0;

    for (int bars = 1; bars <= 2048; ++bars) {
        const double beats = static_cast<double>(bars) * beatsPerBar;
        const double bpm = beats * 60.0 / longestSeconds;
        if (bpm < 60.0 || bpm > 200.0) continue;

        // Most music is an integer tempo; a fair amount is a half.
        const double toWhole = std::abs(bpm - std::round(bpm));
        const double toHalf = std::abs(bpm * 2.0 - std::round(bpm * 2.0)) * 0.5;
        const double error = std::min(toWhole, toHalf);
        if (error > 0.02) continue;

        // Songs come in powers of two far more often than not.
        const bool powerOfTwo = (bars & (bars - 1)) == 0;
        const double barScore = powerOfTwo ? 0.25
                              : (bars % 8 == 0) ? 0.5
                              : (bars % 4 == 0) ? 0.8 : 1.5;

        // Distance from a typical tempo, in octaves - so 64 and 256 are equally
        // unlikely relative to 128, which is what halving and doubling means.
        const double tempoScore = std::abs(std::log2(bpm / 128.0));

        double score = barScore * (1.0 + tempoScore * 2.0) + error * 100.0;
        if (hint > 0.0) score += std::abs(std::log2(bpm / hint)) * 4.0;

        if (score < bestScore) {
            bestScore = score;
            bestBpm = std::round(bpm * 2.0) / 2.0;
            bestBars = bars;
        }
    }

    if (bestBars == 0) return 0.0;

    if (outBars) *outBars = bestBars;
    return bestBpm;
}

// ---------------------------------------------------------------------------
// Spectral overview
// ---------------------------------------------------------------------------

const std::vector<StemPlayerNode::SpectralBand>& StemPlayerNode::spectrum(int slot) const {
    static const std::vector<SpectralBand> empty;
    if (slot < 0 || slot >= kMaxStems) return empty;
    return stems_[static_cast<std::size_t>(slot)].spectrum;
}

void StemPlayerNode::buildSpectrum(const SampleBuffer& buffer, std::vector<SpectralBand>& out,
                                   int buckets) {
    out.clear();
    if (buffer.empty() || buckets <= 0) return;

    out.resize(static_cast<std::size_t>(buckets));

    const std::int64_t frames = buffer.frames();
    const std::int64_t perBucket = std::max<std::int64_t>(1, frames / buckets);
    const double rate = buffer.sampleRate() > 0.0 ? buffer.sampleRate() : 48000.0;

    // Two one-pole splits at roughly 200 Hz and 2 kHz. Crude, but this is a
    // picture of where the weight sits, not a measurement.
    //
    // The filters run on every sample; only the accumulation is decimated.
    // Skipping samples on the way *in* is undersampling, and it folds the top
    // of the spectrum down into the middle - a 9 kHz stem came out reading the
    // same as an 800 Hz one, which is precisely the distinction the strip
    // exists to draw.
    constexpr int kStep = 4;
    const double lowCoeff = std::exp(-2.0 * 3.14159265358979323846 * 200.0 / rate);
    const double highCoeff = std::exp(-2.0 * 3.14159265358979323846 * 2000.0 / rate);

    double lowState = 0.0, highState = 0.0;
    float peak = 0.0f;

    for (int b = 0; b < buckets; ++b) {
        const std::int64_t start = static_cast<std::int64_t>(b) * perBucket;
        const std::int64_t end = std::min(frames, start + perBucket);

        double lowSum = 0.0, midSum = 0.0, highSum = 0.0;
        std::int64_t counted = 0;

        for (std::int64_t i = start; i < end; ++i) {
            double sample = 0.0;
            for (int c = 0; c < buffer.channels(); ++c) sample += buffer.channel(c)[i];
            sample /= std::max(1, buffer.channels());

            lowState = sample + lowCoeff * (lowState - sample);
            highState = sample + highCoeff * (highState - sample);

            if ((i % kStep) != 0) continue;

            const double low = lowState;
            const double mid = highState - lowState;
            const double high = sample - highState;

            lowSum += low * low;
            midSum += mid * mid;
            highSum += high * high;
            ++counted;
        }

        if (counted > 0) {
            SpectralBand& band = out[static_cast<std::size_t>(b)];
            band.low = static_cast<float>(std::sqrt(lowSum / static_cast<double>(counted)));
            band.mid = static_cast<float>(std::sqrt(midSum / static_cast<double>(counted)));
            band.high = static_cast<float>(std::sqrt(highSum / static_cast<double>(counted)));
            peak = std::max(peak, std::max(band.low, std::max(band.mid, band.high)));
        }
    }

    // Normalised against the stem's own peak, so a quiet stem still reads as
    // whatever colour it is rather than as black.
    if (peak > 1.0e-6f) {
        for (SpectralBand& band : out) {
            band.low /= peak;
            band.mid /= peak;
            band.high /= peak;
        }
    }
}

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------

namespace {

// Publishes the message thread's section list as an immutable table the audio
// thread can read without a lock.
std::shared_ptr<StemPlayerNode::SectionTable> makeTable(const std::vector<StemSection>& from);

} // namespace

void StemPlayerNode::addSection(StemSection section) {
    if (static_cast<int>(sections_.size()) >= kMaxSections) return;
    sections_.push_back(std::move(section));
    sectionTable_.publish(makeTable(sections_));
}

void StemPlayerNode::removeSection(int index) {
    if (index < 0 || index >= static_cast<int>(sections_.size())) return;
    sections_.erase(sections_.begin() + index);
    sectionTable_.publish(makeTable(sections_));

    // Keep the selection pointing at something that exists.
    const int count = std::max(1, static_cast<int>(sections_.size()));
    if (activeSection_.load(std::memory_order_relaxed) >= count)
        requestSection(count - 1);
}

void StemPlayerNode::updateSection(int index, const StemSection& section) {
    if (index < 0 || index >= static_cast<int>(sections_.size())) return;
    sections_[static_cast<std::size_t>(index)] = section;
    sectionTable_.publish(makeTable(sections_));
}

void StemPlayerNode::clearSections() {
    sections_.clear();
    sectionTable_.publish(makeTable(sections_));
}

void StemPlayerNode::requestSection(int index) noexcept {
    if (index < 0) return;
    if (pSection_ >= 0) parameter(pSection_).setValue(static_cast<float>(index));
}

void StemPlayerNode::resolveSection(int index, double beatsPerBar, double& startBeats,
                                    double& lengthBeats) const {
    startBeats = 0.0;
    // No sections defined: loop sixteen bars, which is long enough to hear a
    // whole idea and short enough to be obviously a loop.
    lengthBeats = 16.0 * beatsPerBar;

    const SectionTable* table = sectionTable_.get();
    if (!table || table->count <= 0) return;

    const int clamped = clampValue(index, 0, table->count - 1);
    const StemSection& section = table->entries[static_cast<std::size_t>(clamped)];

    startBeats = static_cast<double>(section.startBar) * beatsPerBar;
    lengthBeats = static_cast<double>(std::max(1, section.lengthBars)) * beatsPerBar;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void StemPlayerNode::prepare(const PrepareInfo& info) {
    Node::prepare(info);

    for (Stem& stem : stems_) stem.buffer.setClock(info.blockCounter);
    sectionTable_.setClock(info.blockCounter);
    if (!sectionTable_.get()) sectionTable_.publish(makeTable(sections_));

    for (SmoothedValue& gain : stemGain_) gain.reset(info.sampleRate, 0.01);
    masterGain_.reset(info.sampleRate, 0.01);
    reset();
}

void StemPlayerNode::reset() {
    lastLocalBeats_ = 0.0;
    repeatAnchorBeats_ = -1.0;
    lastDivide_ = 1;
    loopProgress_.store(0.0f, std::memory_order_relaxed);
    for (Stem& stem : stems_) {
        stem.meter[0].store(0.0f, std::memory_order_relaxed);
        stem.meter[1].store(0.0f, std::memory_order_relaxed);
    }
}

void StemPlayerNode::serviceFromMessageThread() {
    for (Stem& stem : stems_) stem.buffer.collect();
    sectionTable_.collect();
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

float StemPlayerNode::readStem(const SampleBuffer& s, int channel, double position) {
    const std::int64_t frames = s.frames();
    if (frames <= 0) return 0.0f;

    if (position < 0.0 || position >= static_cast<double>(frames - 1)) return 0.0f;

    const std::int64_t index = static_cast<std::int64_t>(position);
    const float fraction = static_cast<float>(position - static_cast<double>(index));

    const float* data = s.channel(channel < s.channels() ? channel : s.channels() - 1);
    return data[index] + (data[index + 1] - data[index]) * fraction;
}

void StemPlayerNode::process(ProcessContext& ctx) {
    ctx.clearOutputs();

    const int frames = ctx.frames;
    if (frames <= 0 || ctx.transport == nullptr) return;

    const TransportState& transport = *ctx.transport;
    const double beatsPerBar = static_cast<double>(std::max(1, transport.timeSigNumerator));
    // The stems' own tempo when one is known. A set holding two songs at
    // different tempos needs each stem player reading its own file at the rate
    // it was bounced at, while the transport keeps one grid for the whole patch.
    const double bpm = stemBpm_ > 0.0 ? stemBpm_
                     : (transport.bpm > 0.0 ? transport.bpm : 120.0);

    const bool follow = paramValue(pFollow_) > 0.5f;
    if (follow && !transport.playing) {
        loopProgress_.store(0.0f, std::memory_order_relaxed);
        for (Stem& stem : stems_) {
            stem.meter[0].store(0.0f, std::memory_order_relaxed);
            stem.meter[1].store(0.0f, std::memory_order_relaxed);
        }
        return;
    }

    // -- section selection -------------------------------------------------
    const int requested = static_cast<int>(std::lround(paramValue(pSection_)));
    int active = activeSection_.load(std::memory_order_relaxed);

    if (requested != lastRequestedSection_) {
        lastRequestedSection_ = requested;
        if (requested != active) pendingSection_.store(requested, std::memory_order_relaxed);
    }

    const auto launch = static_cast<Launch>(static_cast<int>(std::lround(paramValue(pLaunch_))));
    if (launch == Launch::Immediate) {
        const int pending = pendingSection_.exchange(-1, std::memory_order_relaxed);
        if (pending >= 0) {
            active = pending;
            activeSection_.store(active, std::memory_order_relaxed);
            repeatAnchorBeats_ = -1.0;
        }
    }

    double startBeats = 0.0, lengthBeats = 0.0;
    resolveSection(active, beatsPerBar, startBeats, lengthBeats);
    if (lengthBeats <= 0.0) return;

    // -- loop divide -------------------------------------------------------
    static constexpr int kDivides[] = { 1, 2, 4, 8, 16, 32 };
    const int divideIndex = clampValue(static_cast<int>(std::lround(paramValue(pDivide_))),
                                       0, static_cast<int>(std::size(kDivides)) - 1);
    const int divide = kDivides[divideIndex];
    const bool repeat = paramValue(pRepeat_) > 0.5f;

    const double divideBeats = lengthBeats / static_cast<double>(divide);

    // -- gains -------------------------------------------------------------
    masterGain_.setTarget(dsp::dbToGain(paramValue(pGain_)));
    for (int slot = 0; slot < kMaxStems; ++slot) {
        const bool muted = paramValue(pStemMute_[static_cast<std::size_t>(slot)]) > 0.5f;
        stemGain_[static_cast<std::size_t>(slot)].setTarget(
            muted ? 0.0f : dsp::dbToGain(paramValue(pStemGain_[static_cast<std::size_t>(slot)])));
    }

    // -- resolve each stem's buffer once for the block ---------------------
    const SampleBuffer* buffers[kMaxStems] = {};
    for (int slot = 0; slot < kMaxStems; ++slot)
        buffers[slot] = stems_[static_cast<std::size_t>(slot)].buffer.get();

    const int mixPort = kMaxStems;   // the summed pair sits after the stems
    float peak[kMaxStems][2] = {};
    const std::uint32_t mask = chopMask_.load(std::memory_order_relaxed);
    double lastPosition[kMaxStems] = {};

    const double beatsPerFrame = bpm / (60.0 * transport.sampleRate);

    for (int i = 0; i < frames; ++i) {
        const double ppq = transport.ppqPosition + static_cast<double>(i) * beatsPerFrame;

        // Where we are inside the section's loop. Derived from the transport
        // rather than accumulated, so it cannot drift however long the set runs
        // and a section change lands exactly on the grid.
        double localBeats = std::fmod(ppq, lengthBeats);
        if (localBeats < 0.0) localBeats += lengthBeats;

        // A wrap is the end of the loop, and the moment a queued section takes
        // over. Also the moment a bar- or beat-quantised launch is checked, so
        // that all four launch modes share one path.
        const bool wrapped = localBeats < lastLocalBeats_;

        // Crossing a grid line is a change of whole-units between this sample
        // and the last, which is exact and needs no epsilon.
        const double previousPpq = ppq - beatsPerFrame;
        const bool barEdge = std::floor(ppq / beatsPerBar) != std::floor(previousPpq / beatsPerBar);
        const bool beatEdge = std::floor(ppq) != std::floor(previousPpq);

        const bool boundary = launch == Launch::EndOfLoop ? wrapped
                            : launch == Launch::Bar ? (barEdge || wrapped)
                            : launch == Launch::Beat ? (beatEdge || wrapped)
                            : false;

        if (boundary) {
            const int pending = pendingSection_.exchange(-1, std::memory_order_relaxed);
            if (pending >= 0) {
                active = pending;
                activeSection_.store(active, std::memory_order_relaxed);
                resolveSection(active, beatsPerBar, startBeats, lengthBeats);
                repeatAnchorBeats_ = -1.0;
                localBeats = std::fmod(ppq, lengthBeats);
                if (localBeats < 0.0) localBeats += lengthBeats;
            }
        }
        lastLocalBeats_ = localBeats;

        // -- repeat ---------------------------------------------------------
        // Latches whichever subdivision was playing when the divide engaged and
        // stutters that one, rather than jumping to the start of the section.
        // Repeating the chunk you are already in is what makes it a build.
        double readBeats = localBeats;
        if (divide > 1) {
            if (repeat) {
                if (repeatAnchorBeats_ < 0.0 || divide != lastDivide_)
                    repeatAnchorBeats_ = std::floor(localBeats / divideBeats) * divideBeats;

                double inChunk = std::fmod(ppq, divideBeats);
                if (inChunk < 0.0) inChunk += divideBeats;
                readBeats = repeatAnchorBeats_ + inChunk;
            } else {
                repeatAnchorBeats_ = -1.0;
            }
        } else {
            repeatAnchorBeats_ = -1.0;
        }
        lastDivide_ = divide;

        const double choppedBeats = startBeats + readBeats;
        const double straightBeats = startBeats + localBeats;
        const double masterLevel = masterGain_.next();

        float mixL = 0.0f, mixR = 0.0f;

        for (int slot = 0; slot < kMaxStems; ++slot) {
            const float gain = stemGain_[static_cast<std::size_t>(slot)].next()
                             * static_cast<float>(masterLevel);

            const SampleBuffer* buffer = buffers[slot];
            if (buffer == nullptr || buffer->empty()) continue;

            // Only the stems in the chop mask follow the repeat; the rest read
            // straight through, so a stuttered drum loop can sit under a pad
            // that is still playing the bar out.
            const bool chopped = (mask & (1u << slot)) != 0;
            const double songBeats = chopped ? choppedBeats : straightBeats;

            // Each stem is read at its own file rate, so a set exported at
            // 44.1 and a riser at 48 sit together without a resample pass.

            const double position = songBeats * 60.0 / bpm * buffer->sampleRate();

            const float left = readStem(*buffer, 0, position) * gain;
            const float right = readStem(*buffer, 1, position) * gain;

            AudioBus& out = ctx.output(slot);
            if (out.numChannels > 0) out.chan(0)[i] = left;
            if (out.numChannels > 1) out.chan(1)[i] = right;

            lastPosition[slot] = position / static_cast<double>(std::max<std::int64_t>(1, buffer->frames()));

            mixL += left;
            mixR += right;

            const float absL = left < 0.0f ? -left : left;
            const float absR = right < 0.0f ? -right : right;
            if (absL > peak[slot][0]) peak[slot][0] = absL;
            if (absR > peak[slot][1]) peak[slot][1] = absR;
        }

        AudioBus& mix = ctx.output(mixPort);
        if (mix.numChannels > 0) mix.chan(0)[i] = mixL;
        if (mix.numChannels > 1) mix.chan(1)[i] = mixR;

        if (i == frames - 1)
            loopProgress_.store(static_cast<float>(localBeats / lengthBeats),
                                std::memory_order_relaxed);
    }

    for (int slot = 0; slot < kMaxStems; ++slot) {
        playhead_[static_cast<std::size_t>(slot)].store(
            static_cast<float>(clampValue(lastPosition[slot], 0.0, 1.0)),
            std::memory_order_relaxed);
    }

    // Meters fall back at roughly the same rate as the master's, so a stem grid
    // reads consistently against the transport bar.
    const float decay = std::exp(-static_cast<float>(frames)
                                 / static_cast<float>(ctx.sampleRate * 0.25));
    for (int slot = 0; slot < kMaxStems; ++slot) {
        for (int c = 0; c < 2; ++c) {
            std::atomic<float>& meter = stems_[static_cast<std::size_t>(slot)].meter[c];
            const float previous = meter.load(std::memory_order_relaxed) * decay;
            meter.store(peak[slot][c] > previous ? peak[slot][c] : previous,
                        std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

void StemPlayerNode::saveExtraState(JsonValue& out) const {
    JsonValue stems = JsonValue::array();
    for (int slot = 0; slot < kMaxStems; ++slot) {
        JsonValue entry = JsonValue::object();
        entry.set("slot", slot);
        entry.set("path", stems_[static_cast<std::size_t>(slot)].path);
        entry.set("name", stems_[static_cast<std::size_t>(slot)].name);
        stems.push(entry);
    }
    out.set("stems", stems);

    JsonValue sections = JsonValue::array();
    for (const StemSection& section : sections_) {
        JsonValue entry = JsonValue::object();
        entry.set("name", section.name);
        entry.set("startBar", section.startBar);
        entry.set("lengthBars", section.lengthBars);
        entry.set("hue", section.hue);
        sections.push(entry);
    }
    out.set("sections", sections);
    out.set("stemBpm", stemBpm_);
    out.set("tempoSource", tempoSource_);
}

void StemPlayerNode::loadExtraState(const JsonValue& in) {
    if (const JsonValue* stems = in.find("stems"); stems && stems->isArray()) {
        for (std::size_t i = 0; i < stems->size(); ++i) {
            const JsonValue& entry = stems->at(i);
            const int slot = entry.getInt("slot", static_cast<int>(i));
            if (slot < 0 || slot >= kMaxStems) continue;

            const std::string name = entry.getString("name");
            if (!name.empty()) stems_[static_cast<std::size_t>(slot)].name = name;

            // A stem that has moved or gone is reported on the node rather than
            // failing the load: the sections and the mix are still worth having,
            // and the path can be re-pointed from the editor.
            const std::string path = entry.getString("path");
            if (!path.empty()) loadStem(slot, path, nullptr);
        }
    }

    stemBpm_ = in.getDouble("stemBpm", 0.0);
    tempoSource_ = in.getString("tempoSource", "project");

    sections_.clear();
    if (const JsonValue* sections = in.find("sections"); sections && sections->isArray()) {
        for (std::size_t i = 0; i < sections->size() && i < kMaxSections; ++i) {
            const JsonValue& entry = sections->at(i);
            StemSection section;
            section.name = entry.getString("name", "section");
            section.startBar = entry.getInt("startBar", 0);
            section.lengthBars = std::max(1, entry.getInt("lengthBars", 8));
            section.hue = entry.getFloat("hue", 0.0f);
            sections_.push_back(std::move(section));
        }
    }
    sectionTable_.publish(makeTable(sections_));
}

namespace {

std::shared_ptr<StemPlayerNode::SectionTable> makeTable(const std::vector<StemSection>& from) {
    auto table = std::make_shared<StemPlayerNode::SectionTable>();
    table->count = std::min(static_cast<int>(from.size()), kMaxSections);
    for (int i = 0; i < table->count; ++i)
        table->entries[static_cast<std::size_t>(i)] = from[static_cast<std::size_t>(i)];
    return table;
}

} // namespace

} // namespace acm

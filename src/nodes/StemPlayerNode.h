// Stem player with a musical section selector.
//
// The unit of performance here is a *section* of a song - an intro, a chorus,
// eight bars of a breakdown - played by however many stems the song was
// exported as. Every stem is the full song; a section is a bar range into it.
// Looping a section means reading each stem at that bar range while the
// transport keeps running underneath, so the loop is always locked to the grid
// and never drifts.
//
// Switching sections is deferred. Ask for a new one at any point and the current
// loop plays out to its end before the change takes effect, which is what makes
// it usable as a performance instrument: you commit to the next section whenever
// you notice you want it, and it lands in time. This is the behaviour AudioMulch
// gets right and most clip launchers get wrong by default.
//
// The section parameter is stepped rather than continuous, so a metasurface
// blend snaps to the nearest snapshot's section instead of trying to average
// two of them, which would be meaningless.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/AtomicResource.h"
#include "../core/Node.h"
#include "../dsp/Dsp.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace acm {

// Eight is the number of stems a song is usually bounced to - drums, bass,
// two or three synth layers, vocals, and a bus for everything else. Fixed
// rather than dynamic because the port count is part of the node's identity in
// a saved patch, and a stem count that changed underneath a patch would break
// every cable into it.
inline constexpr int kMaxStems = 8;

// Sixteen named sections is more than any song has and few enough to pick from
// a grid on a touchscreen without scrolling.
inline constexpr int kMaxSections = 16;

struct StemSection {
    std::string name = "section";
    int startBar = 0;
    int lengthBars = 8;
    // Drawn on the section grid and carried onto the metasurface, so a set can
    // be read as shape and colour rather than by reading labels.
    float hue = 0.0f;
};

class StemPlayerNode : public Node {
public:
    StemPlayerNode();

    // -- stems (message thread) --------------------------------------------
    bool loadStem(int slot, const std::string& utf8Path, std::string* error = nullptr);
    void clearStem(int slot);
    std::shared_ptr<SampleBuffer> stem(int slot) const;
    const std::string& stemPath(int slot) const;
    const std::string& stemName(int slot) const;
    void setStemName(int slot, std::string name);
    bool stemLoaded(int slot) const;

    // The longest loaded stem, which is what the section editor lays bars out
    // against. Zero when nothing is loaded.
    double songLengthBars(double bpm, int beatsPerBar) const;

    // -- sections (message thread) -----------------------------------------
    const std::vector<StemSection>& sections() const noexcept { return sections_; }
    int sectionCount() const noexcept { return static_cast<int>(sections_.size()); }
    void addSection(StemSection section);
    void removeSection(int index);
    void updateSection(int index, const StemSection& section);
    void clearSections();

    // -- performance -------------------------------------------------------
    // Asks for a section. Takes effect at the next loop boundary unless the
    // launch quantise says sooner. Safe from any thread.
    void requestSection(int index) noexcept;
    // Which section is sounding, and which one is queued behind it (-1 = none).
    int activeSection() const noexcept { return activeSection_.load(std::memory_order_relaxed); }
    int pendingSection() const noexcept { return pendingSection_.load(std::memory_order_relaxed); }

    // Position through the current loop, 0..1, for the progress ring.
    float loopProgress() const noexcept { return loopProgress_.load(std::memory_order_relaxed); }
    float meterLevel(int slot, int channel) const noexcept;

    // -- Node --------------------------------------------------------------
    void prepare(const PrepareInfo& info) override;
    void reset() override;
    void process(ProcessContext& ctx) override;
    void serviceFromMessageThread() override;
    void saveExtraState(JsonValue& out) const override;
    void loadExtraState(const JsonValue& in) override;

    // Referenced by the inline editor and the glitch node.
    enum class Launch : int { EndOfLoop = 0, Bar, Beat, Immediate };

    // Public because the immutable snapshot handed to the audio thread has to be
    // constructible from outside the class.
    struct SectionTable {
        std::array<StemSection, kMaxSections> entries;
        int count = 0;
    };

    // Parameter ids the glitch generator drives. Named here so the two nodes
    // agree on them without either one reaching into the other.
    static constexpr const char* kSectionParam = "section";
    static constexpr const char* kDivideParam = "divide";
    static constexpr const char* kRepeatParam = "repeat";

private:
    struct Stem {
        AtomicResource<SampleBuffer> buffer;
        std::string path;
        std::string name;
        std::atomic<float> meter[2] = {};
    };

    // Reads one stem at a fractional song position, with linear interpolation.
    static float readStem(const SampleBuffer& s, int channel, double position);

    // Section bounds in beats. Falls back to the whole song when there are no
    // sections, so a freshly loaded set of stems plays rather than sits silent.
    void resolveSection(int index, double beatsPerBar, double& startBeats,
                        double& lengthBeats) const;

    std::array<Stem, kMaxStems> stems_;

    // Message-thread copy, and the immutable snapshot the audio thread reads.
    std::vector<StemSection> sections_;
    AtomicResource<SectionTable> sectionTable_;

    ParamIndex pSection_ = -1, pLaunch_ = -1, pDivide_ = -1, pRepeat_ = -1;
    ParamIndex pGain_ = -1, pFollow_ = -1;
    std::array<ParamIndex, kMaxStems> pStemGain_{};
    std::array<ParamIndex, kMaxStems> pStemMute_{};

    std::atomic<int> activeSection_{ 0 };
    std::atomic<int> pendingSection_{ -1 };
    std::atomic<float> loopProgress_{ 0.0f };

    // Audio-thread only.
    double lastLocalBeats_ = 0.0;
    int lastRequestedSection_ = 0;
    double repeatAnchorBeats_ = -1.0;
    int lastDivide_ = 1;

    std::array<SmoothedValue, kMaxStems> stemGain_;
    SmoothedValue masterGain_;
};

} // namespace acm

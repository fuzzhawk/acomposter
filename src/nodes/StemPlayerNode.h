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
#include <utility>
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
    // Takes an already-decoded buffer. Used by the tests, and by anything that
    // has audio in hand rather than a path.
    void setStemFromBuffer(int slot, std::shared_ptr<SampleBuffer> buffer, std::string name);
    std::shared_ptr<SampleBuffer> stem(int slot) const;
    const std::string& stemPath(int slot) const;
    const std::string& stemName(int slot) const;
    void setStemName(int slot, std::string name);
    bool stemLoaded(int slot) const;

    // The longest loaded stem, which is what the section editor lays bars out
    // against. Zero when nothing is loaded.
    double songLengthBars(double bpm, int beatsPerBar) const;

    // -- snippet -----------------------------------------------------------
    // A range dragged out on a stem's spectral strip, to be handed to the build
    // generator. Held here rather than on the build node because it is selected
    // in context - against the waveform of the stem it comes from - and because
    // one selection can be sent to several places.
    struct Snippet {
        int slot = -1;
        double startSeconds = 0.0;
        double lengthSeconds = 0.0;
        // Rounded to a whole number of beats at the stem tempo. A grain cloud
        // built from a loop that is not a loop drifts against everything else.
        bool tempoMatched = true;

        bool valid() const noexcept { return slot >= 0 && lengthSeconds > 0.0; }
    };

    const Snippet& snippet() const noexcept { return snippet_; }
    void setSnippet(const Snippet& snippet);
    void clearSnippet() { snippet_ = Snippet{}; }

    // The selection as audio, copied out so whoever receives it owns it and can
    // read it on the audio thread without reaching back into this node.
    std::shared_ptr<SampleBuffer> extractSnippet(int beatsPerBar) const;

    // -- routing -----------------------------------------------------------
    // Which output a stem lands on. Several stems can share one, which is the
    // point: a tag is a category, and a category is a bus.
    //
    // A stem's tag decides its output by default, so tagging a folder of stems
    // routes them without any further work. An explicit route overrides that,
    // because the first time a song has two things tagged "pads" that need
    // separate treatment, a rule with no exception is a rule you have to fight.
    void setStemTag(int slot, std::string tagId);
    const std::string& stemTag(int slot) const;

    // -1 means "follow the tag".
    void setStemRoute(int slot, int output);
    int stemRoute(int slot) const;
    // Where the stem actually goes, after the tag and the override are resolved.
    int resolvedRoute(int slot) const;

    // Told by the application when the palette changes, so the node can resolve
    // tags to outputs without knowing what a library is.
    void setTagRouting(const std::vector<std::pair<std::string, int>>& tagToOutput);

    // Drawn open, the matrix panel hangs off the right of the node. A view
    // preference, so it travels with the patch.
    bool matrixOpen = false;

    // -- tempo -------------------------------------------------------------
    // The tempo the stems were bounced at. Everything the node does with the
    // grid is in terms of this, not the project tempo, so a set can hold songs
    // at different tempos without each one needing its own transport.
    double stemBpm() const noexcept { return stemBpm_; }
    void setStemBpm(double bpm) noexcept;

    // Works the length back into a tempo, assuming the longest stem is a whole
    // number of bars - which a bounced stem essentially always is. Returns 0
    // when it cannot find a musically plausible answer.
    double detectBpm(int beatsPerBar, int* outBars = nullptr) const;
    // Whether the tempo came from detection, a file name, or the user.
    const std::string& tempoSource() const noexcept { return tempoSource_; }

    // -- spectral overview -------------------------------------------------
    // Three bands per bucket - low, mid, high - normalised against the stem's
    // own peak. Drawn as colour so a glance at the node says which stem carries
    // the weight and which carries the air.
    struct SpectralBand { float low = 0.0f; float mid = 0.0f; float high = 0.0f; };
    const std::vector<SpectralBand>& spectrum(int slot) const;

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

    // Where each stem is reading, as a fraction of its own file. Per stem
    // rather than one shared value, because a chopped stem is deliberately not
    // where the others are - that difference is the effect, and seeing it is
    // how you know the chop is on.
    float stemPlayhead(int slot) const noexcept;

    // Which stems the build node's beat repeat applies to, one bit per slot.
    // Everything not in the mask keeps playing straight through, which is what
    // makes a chop musical: stuttering the drums over a held pad is an effect,
    // stuttering everything is a fault.
    void setChopMask(std::uint32_t mask) noexcept {
        chopMask_.store(mask, std::memory_order_relaxed);
    }
    std::uint32_t chopMask() const noexcept { return chopMask_.load(std::memory_order_relaxed); }
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
        // Message-thread only, built once on load.
        std::vector<SpectralBand> spectrum;
    };

    // Reads one stem at a fractional song position, with linear interpolation.
    static float readStem(const SampleBuffer& s, int channel, double position);

    // Section bounds in beats. Falls back to the whole song when there are no
    // sections, so a freshly loaded set of stems plays rather than sits silent.
    void resolveSection(int index, double beatsPerBar, double& startBeats,
                        double& lengthBeats) const;

    // Splits a stem into low/mid/high energy per bucket with a pair of
    // one-pole filters. Cheap, and enough to colour a strip: this is a picture,
    // not an analyser.
    static void buildSpectrum(const SampleBuffer& buffer, std::vector<SpectralBand>& out,
                              int buckets);

    std::array<Stem, kMaxStems> stems_;

    // Message-thread routing state, published to the audio thread as a plain
    // array of ints - small enough that a torn read cannot do worse than send
    // one block of one stem to the wrong bus.
    Snippet snippet_;
    std::array<std::string, kMaxStems> stemTag_;
    std::array<int, kMaxStems> routeOverride_{};
    std::array<std::atomic<int>, kMaxStems> resolvedRoute_{};
    std::vector<std::pair<std::string, int>> tagRouting_;

    void republishRouting();

    double stemBpm_ = 0.0;          // 0 = follow the project tempo
    std::string tempoSource_ = "project";

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
    std::atomic<std::uint32_t> chopMask_{ 0xFFFFFFFFu };
    std::array<std::atomic<float>, kMaxStems> playhead_{};

    // Audio-thread only.
    double lastLocalBeats_ = 0.0;
    int lastRequestedSection_ = 0;
    double repeatAnchorBeats_ = -1.0;
    int lastDivide_ = 1;

    std::array<SmoothedValue, kMaxStems> stemGain_;
    SmoothedValue masterGain_;
};

} // namespace acm

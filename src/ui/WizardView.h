// The analyse-and-rebuild wizard: a folder of badly-named files, one screen at
// a time, into a folder that can be worked with.
//
// It proposes rather than acts. Every file gets a guessed instrument, a
// proposed name and a tag, each shown with the reason it was guessed, and
// nothing happens until it is approved. That asymmetry is the whole design: a
// classifier built on eight band energies and an envelope is going to be wrong
// often enough that acting first and asking later would mean a folder nobody
// can trust and no way to tell which parts were the machine's fault.
//
// It also never moves or overwrites. Approved files are *copied* to a new
// folder under their new names; the originals stay exactly where they were.
// Rebuilding a sample library is the kind of operation people do once, late, on
// a deadline, and it has to be impossible to lose anything doing it.
#pragma once

#include "../core/Engine.h"
#include "../library/Classify.h"
#include "../library/FileIndex.h"
#include "../library/Library.h"
#include "../library/Slicer.h"
#include "Ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acm::ui {

class WizardView {
public:
    void initialise(Engine* engine, library::Library* library);

    // `index` is the librarian's, already scanned. The wizard reads it and
    // never writes to it.
    void begin(const library::FileIndex* index, std::string destinationFolder);
    bool active() const noexcept { return active_; }
    void close() noexcept { active_ = false; }

    void render(Ui& ui, const Rect& bounds);

    // Raised after files are written, so the librarian can pick them up.
    std::function<void()> onFolderChanged;

private:
    struct Proposal {
        const library::IndexedFile* file = nullptr;
        library::Classification guess;
        std::string proposedName;
        std::string tagId;
        bool approved = false;
        bool rejected = false;
    };

    void buildProposals();
    void drawHeader(Ui& ui, Rect& area);
    void drawCurrent(Ui& ui, Rect& area);
    void drawSimilarGroup(Ui& ui, Rect& area);
    void drawSlices(Ui& ui, Rect& area);
    void drawFooter(Ui& ui, Rect& area);

    // Writes every approved proposal into the destination. Returns how many
    // files were written.
    int applyApproved(Ui& ui);

    Proposal* current();
    void step(int delta);
    void loadCurrent();

    Engine* engine_ = nullptr;
    library::Library* library_ = nullptr;
    const library::FileIndex* index_ = nullptr;

    bool active_ = false;
    std::string destination_;

    std::vector<Proposal> proposals_;
    int position_ = 0;

    // The file the cursor is on, loaded for audition and slicing.
    std::shared_ptr<SampleBuffer> loaded_;
    std::vector<std::int64_t> slicePoints_;
    bool sliceMode_ = false;
    float sliceSensitivity_ = 1.5f;

    std::string nameBuffer_;
    int nameBufferFor_ = -1;

    // How alike two files have to be for "approve all like this" to include
    // them. Shown as a percentage, because that is what the number means.
    float groupThreshold_ = 0.82f;
};

} // namespace acm::ui

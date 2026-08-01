// The file librarian: a folder of samples, analysed, searchable and auditioned.
//
// A folder of ten thousand one-shots is unusable by name. Names are
// inconsistent, often wrong, and say nothing about what a sound is. So the
// librarian sorts and filters by what the analysis found - how long, how
// bright, what note, how loud - and can be asked the question that actually
// comes up while working: what else in here sounds like this one.
//
// Everything it offers to change a file writes a new file. Normalising and
// trimming produce a sibling with a suffix; the original is never touched,
// because a librarian that can damage the library is not one anybody will point
// at their samples folder.
#pragma once

#include "../core/Engine.h"
#include "../library/FileIndex.h"
#include "../library/Library.h"
#include "Ui.h"
#include "WizardView.h"

#include <functional>
#include <memory>
#include <string>

namespace acm::ui {

class LibrarianView {
public:
    void initialise(Engine* engine, library::Library* library);

    void render(Ui& ui, const Rect& bounds);
    // Publishes a finished scan. Called every frame, cheap when idle.
    void serviceFromMessageThread();

    // Points the librarian at a folder and scans it.
    void openFolder(std::string utf8Path);

    std::function<std::string()> onBrowseForFolder;
    std::function<void(const std::string& path)> onSendToPatch;

private:
    void drawToolbar(Ui& ui, Rect& area);
    void drawFilters(Ui& ui, Rect& area);
    void drawList(Ui& ui, const Rect& area);
    void drawDetail(Ui& ui, const Rect& area);
    void drawSpectral3D(Ui& ui, const Rect& area);
    void drawBands(Ui& ui, const Rect& area, const library::Analysis& analysis);

    void select(const std::string& path);
    // Writes a processed copy beside the original and returns its path.
    std::string writeProcessedCopy(const std::string& suffix, bool normalise, bool trim);

    Engine* engine_ = nullptr;
    library::Library* library_ = nullptr;
    library::FileIndex index_;

    // The wizard takes over the whole tab while it is running, because working
    // through four hundred proposals in a side panel is not working through
    // them.
    WizardView wizard_;

    std::string folder_;
    std::string selected_;
    // The reference for a similarity sort, which is not always the selection:
    // pressing "find similar" pins one file and then the list can be browsed.
    std::string similarTo_;

    library::Filter filter_;
    library::SortKey sortKey_ = library::SortKey::Name;
    bool sortDescending_ = false;

    // The selected file, loaded for the waveform, the spectral view and the
    // preview. Held rather than reloaded per frame.
    std::shared_ptr<SampleBuffer> loaded_;
    library::Spectrogram spectrogram_;

    // Rotation of the 3D view, dragged with the mouse.
    float spinAngle_ = 0.62f;
    float tiltAngle_ = 0.42f;

    // True for the duration of a drag out of the window, so one press starts one
    // drag rather than a new one on every frame the pointer is still moving.
    bool draggingOut_ = false;

    std::string filterBuffer_;
    // -128 is "any key"; otherwise a semitone index from A4.
    int keyFilter_ = -128;
};

} // namespace acm::ui

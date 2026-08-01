// The stem browser: audition and tag audio before any of it reaches a patch.
//
// This is a *library* tab, not a patch tab. Nothing it does touches the graph,
// and its state survives a patch being loaded or closed, because the whole point
// is to prepare material that several patches will go on to use.
//
// The workflow it exists for: drop a folder of stems in, listen to each one with
// a scrubable playhead, and give it a tag. The tag is what later decides which
// stem player output the file lands on and which effect rack it inherits, so
// doing it once here is what stops it being done by hand in every song.
#pragma once

#include "../audio/SampleBuffer.h"
#include "../core/Engine.h"
#include "../core/FileIo.h"
#include "../library/Library.h"
#include "Ui.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acm::ui {

class StemBrowserView {
public:
    void initialise(Engine* engine, library::Library* library);

    void render(Ui& ui, const Rect& bounds);

    void navigateTo(const std::string& directory);
    const std::string& currentDirectory() const noexcept { return directory_; }

    // Raised when a file is sent to a patch, so the application can decide what
    // to do with it. The browser never touches the graph itself.
    std::function<void(const std::string& path, const std::string& tagId)> onSendToPatch;

private:
    void refresh();
    void select(int index);
    void drawWaveform(Ui& ui, const Rect& bounds);
    void drawTagPalette(Ui& ui, Rect& area);
    void drawFileList(Ui& ui, const Rect& bounds);

    Engine* engine_ = nullptr;
    library::Library* library_ = nullptr;

    std::string directory_;
    std::vector<DirectoryEntry> files_;
    std::string filter_;
    bool needsRefresh_ = true;

    int selected_ = -1;
    // The decoded file behind the waveform. Held here rather than handed to the
    // engine alone, because the view needs it to draw whether or not it is
    // sounding.
    std::shared_ptr<SampleBuffer> loaded_;
    std::string loadedPath_;
    std::string loadError_;

    // Where the playhead sits when nothing is playing, in seconds. Dragging it
    // seeks the audition rather than only moving a marker.
    double playhead_ = 0.0;

    // Tag editing.
    int editingTag_ = -1;
    std::string tagNameBuffer_;
    bool showTagEditor_ = false;
};

} // namespace acm::ui

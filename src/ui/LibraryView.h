// The projects and songs tabs.
//
// One class serves both, because a project and a song differ in two details -
// a project has a running order and a song has lyrics - and everything else
// about them is the same: a name, some notes, a tempo, a key, tags, and a list
// of audio files that belong to it. Two nearly-identical views would drift.
//
// Notes and lyrics are edited a line at a time rather than in a text area. That
// is a real choice and not only an expedient one: a lyric sheet *is* a list of
// lines, and being able to reorder and retitle one line without disturbing the
// rest is more useful here than free-form wrapping would be. It also means the
// existing text field carries all of the editing behaviour instead of a second
// implementation of carets and selection.
//
// Nothing in this view moves or deletes audio. Removing a file from a song
// removes the reference; the file stays where it is, and the library can say
// which other entries still point at it.
#pragma once

#include "../core/Engine.h"
#include "../library/Library.h"
#include "Ui.h"

#include <functional>
#include <string>
#include <vector>

namespace acm::ui {

class LibraryView {
public:
    void initialise(Engine* engine, library::Library* library, library::EntryKind kind);

    void render(Ui& ui, const Rect& bounds);

    // Raised when a file in an entry is asked to be opened on the canvas.
    std::function<void(const std::string& path)> onSendToPatch;

private:
    void drawList(Ui& ui, const Rect& area);
    void drawDetail(Ui& ui, const Rect& area);

    // The shared middle of both kinds: name, tempo, key, tags.
    void drawHeader(Ui& ui, Rect& area, library::Entry& entry);
    void drawFiles(Ui& ui, Rect& area, library::Entry& entry);
    void drawMembers(Ui& ui, Rect& area, library::Entry& entry);
    // `text` is the entry's notes or lyrics; edited as lines and joined back.
    void drawLines(Ui& ui, Rect& area, library::Entry& entry, std::string& text,
                   const char* caption, int salt);

    library::Entry* selectedEntry();

    Engine* engine_ = nullptr;
    library::Library* library_ = nullptr;
    library::EntryKind kind_ = library::EntryKind::Song;

    std::string selected_;
    std::string filter_;

    // Edit buffers, keyed by which entry they belong to so switching selection
    // never carries a half-typed name onto the next one.
    std::string nameBuffer_;
    std::string keyBuffer_;
    std::string bpmBuffer_;
    std::string bufferFor_;

    // The line being edited, as "which text" and "which line" - notes and lyrics
    // share the widget, so the field id has to distinguish them.
    int editingSalt_ = -1;
    int editingLine_ = -1;
    std::string lineBuffer_;
};

} // namespace acm::ui

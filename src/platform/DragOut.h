// Dragging files out of acomposter into other applications.
//
// The librarian and the stem browser both sit on top of real files on disk, and
// the obvious thing to do with a file you have just found or just rendered is
// to drag it into whatever else is open - a DAW, an editor, a folder. Windows
// has one way of expressing that, and it is not the one the application already
// uses for drags coming *in*: WM_DROPFILES delivers a drop, but it cannot start
// one. Starting one means OLE, an IDataObject offering CF_HDROP, and an
// IDropSource that watches the mouse.
//
// Both are written out by hand here rather than pulled in from a framework,
// which is the same bargain the rest of the application makes. They are small:
// the data object serves exactly one format and refuses everything else, which
// is all a file drag needs.
//
// The call blocks. That is inherent - DoDragDrop runs its own modal loop until
// the button comes up - so the window stops redrawing for the length of the
// drag, exactly as it does in every other application that offers this.
#pragma once

#include <string>
#include <vector>

namespace acm::platform {

// Starts a drag of `utf8Paths` as files. Returns true if something accepted the
// drop. Only ever a copy: dragging a sample into a DAW must not move the file
// out of the folder the librarian is indexing.
//
// OLE must already be initialised on the calling thread, which the entry point
// does once for the life of the process - doing it per drag would tear the OLE
// layer back down between drags for no gain.
bool dragOutFiles(const std::vector<std::string>& utf8Paths);

// The path block a CF_HDROP carries: every path terminated, and one more
// terminator to end the list. Exposed because the double terminator is the one
// part of this that is easy to get wrong and possible to test without a drop
// target on the other end - a list missing it makes the last path unreadable,
// or worse, readable plus whatever followed it in memory.
std::wstring dropFileList(const std::vector<std::string>& utf8Paths);

} // namespace acm::platform

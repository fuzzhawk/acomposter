// Native file and folder pickers.
//
// Uses the modern IFileDialog rather than GetOpenFileName, because the same
// interface handles folder selection (which the plugin manager needs) and
// because it respects the user's places and recent locations.
#pragma once

#include <string>
#include <vector>

namespace acm::platform {

struct FileFilter {
    std::string description;   // "acomposter patch"
    std::string pattern;       // "*.acp" or "*.wav;*.aiff"
};

// All of these return an empty string when the user cancels. `owner` is an HWND.
std::string openFileDialog(void* owner, const std::string& title,
                           const std::vector<FileFilter>& filters,
                           const std::string& initialDirectory = {});

std::string saveFileDialog(void* owner, const std::string& title,
                           const std::vector<FileFilter>& filters,
                           const std::string& initialDirectory = {},
                           const std::string& suggestedName = {},
                           const std::string& defaultExtension = {});

std::string pickFolderDialog(void* owner, const std::string& title,
                             const std::string& initialDirectory = {});

// A blocking message box. Returns true for yes/ok.
bool confirmDialog(void* owner, const std::string& title, const std::string& message);
void messageDialog(void* owner, const std::string& title, const std::string& message,
                   bool isError = false);

// Three-way: save / discard / cancel, for the unsaved-changes prompt.
enum class SaveChangesResult { Save, Discard, Cancel };
SaveChangesResult askToSaveChanges(void* owner, const std::string& patchName);

} // namespace acm::platform

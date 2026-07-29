#include "FileDialog.h"

#include "../core/Utf.h"

#include <windows.h>
#include <shobjidl.h>

namespace acm::platform {
namespace {

// Filters are handed to COM as an array of pointers into strings we have to keep
// alive for the duration of the call, hence the parallel storage.
struct FilterStorage {
    std::vector<std::wstring> descriptions;
    std::vector<std::wstring> patterns;
    std::vector<COMDLG_FILTERSPEC> specs;
};

FilterStorage buildFilters(const std::vector<FileFilter>& filters) {
    FilterStorage storage;
    storage.descriptions.reserve(filters.size() + 1);
    storage.patterns.reserve(filters.size() + 1);

    for (const FileFilter& filter : filters) {
        storage.descriptions.push_back(utf8ToWide(filter.description));
        storage.patterns.push_back(utf8ToWide(filter.pattern));
    }

    // Always offer "everything", because a sample with an unexpected extension
    // is still worth trying to open.
    storage.descriptions.push_back(L"all files");
    storage.patterns.push_back(L"*.*");

    for (std::size_t i = 0; i < storage.descriptions.size(); ++i)
        storage.specs.push_back(COMDLG_FILTERSPEC{ storage.descriptions[i].c_str(),
                                                   storage.patterns[i].c_str() });
    return storage;
}

void applyInitialDirectory(IFileDialog* dialog, const std::string& directory) {
    if (directory.empty()) return;

    IShellItem* item = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(utf8ToWide(directory).c_str(), nullptr,
                                                IID_PPV_ARGS(&item)))) {
        // SetDefaultFolder rather than SetFolder: the latter overrides wherever
        // the user last navigated to, which is rude on the second use.
        dialog->SetDefaultFolder(item);
        item->Release();
    }
}

std::string resultPath(IFileDialog* dialog) {
    IShellItem* item = nullptr;
    if (FAILED(dialog->GetResult(&item)) || !item) return {};

    PWSTR path = nullptr;
    std::string result;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
        result = wideToUtf8(path);
        ::CoTaskMemFree(path);
    }
    item->Release();
    return result;
}

// The dialogs may be called from a thread that has not initialised COM.
struct ScopedCom {
    bool owns = false;
    ScopedCom() { owns = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ScopedCom() { if (owns) ::CoUninitialize(); }
};

} // namespace

std::string openFileDialog(void* owner, const std::string& title,
                           const std::vector<FileFilter>& filters,
                           const std::string& initialDirectory) {
    ScopedCom com;

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))))
        return {};

    const FilterStorage storage = buildFilters(filters);
    dialog->SetFileTypes(static_cast<UINT>(storage.specs.size()), storage.specs.data());
    dialog->SetTitle(utf8ToWide(title).c_str());
    applyInitialDirectory(dialog, initialDirectory);

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);

    std::string path;
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner))))
        path = resultPath(dialog);

    dialog->Release();
    return path;
}

std::string saveFileDialog(void* owner, const std::string& title,
                           const std::vector<FileFilter>& filters,
                           const std::string& initialDirectory,
                           const std::string& suggestedName,
                           const std::string& defaultExtension) {
    ScopedCom com;

    IFileSaveDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))))
        return {};

    const FilterStorage storage = buildFilters(filters);
    dialog->SetFileTypes(static_cast<UINT>(storage.specs.size()), storage.specs.data());
    dialog->SetTitle(utf8ToWide(title).c_str());
    applyInitialDirectory(dialog, initialDirectory);

    if (!suggestedName.empty()) dialog->SetFileName(utf8ToWide(suggestedName).c_str());
    if (!defaultExtension.empty()) {
        // Without the leading dot, which is what this call expects.
        const std::string extension = defaultExtension.front() == '.'
                                          ? defaultExtension.substr(1) : defaultExtension;
        dialog->SetDefaultExtension(utf8ToWide(extension).c_str());
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST);

    std::string path;
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner))))
        path = resultPath(dialog);

    dialog->Release();
    return path;
}

std::string pickFolderDialog(void* owner, const std::string& title,
                             const std::string& initialDirectory) {
    ScopedCom com;

    IFileOpenDialog* dialog = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog))))
        return {};

    dialog->SetTitle(utf8ToWide(title).c_str());
    applyInitialDirectory(dialog, initialDirectory);

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);

    std::string path;
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner))))
        path = resultPath(dialog);

    dialog->Release();
    return path;
}

bool confirmDialog(void* owner, const std::string& title, const std::string& message) {
    return ::MessageBoxW(static_cast<HWND>(owner), utf8ToWide(message).c_str(),
                         utf8ToWide(title).c_str(),
                         MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void messageDialog(void* owner, const std::string& title, const std::string& message,
                   bool isError) {
    ::MessageBoxW(static_cast<HWND>(owner), utf8ToWide(message).c_str(),
                  utf8ToWide(title).c_str(),
                  MB_OK | (isError ? MB_ICONERROR : MB_ICONINFORMATION));
}

SaveChangesResult askToSaveChanges(void* owner, const std::string& patchName) {
    const std::wstring message = utf8ToWide("\"" + patchName + "\" has unsaved changes.\n\n"
                                            "Save before closing?");

    const int result = ::MessageBoxW(static_cast<HWND>(owner), message.c_str(),
                                     L"acomposter",
                                     MB_YESNOCANCEL | MB_ICONWARNING);

    switch (result) {
        case IDYES: return SaveChangesResult::Save;
        case IDNO:  return SaveChangesResult::Discard;
        default:    return SaveChangesResult::Cancel;
    }
}

} // namespace acm::platform

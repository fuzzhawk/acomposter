#include "AppPaths.h"

#include "FileIo.h"
#include "Utf.h"

#include <algorithm>
#include <iterator>

#include <windows.h>
#include <shlobj.h>

namespace acm::paths {
namespace {

std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR wide = nullptr;
    if (::SHGetKnownFolderPath(id, 0, nullptr, &wide) != S_OK || wide == nullptr) {
        if (wide) ::CoTaskMemFree(wide);
        return {};
    }
    std::string result = wideToUtf8(wide);
    ::CoTaskMemFree(wide);
    return result;
}

// Reads a REG_SZ value; returns empty when it is missing.
std::string registryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                           DWORD extraFlags) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_READ | extraFlags, &key) != ERROR_SUCCESS)
        return {};

    wchar_t buffer[1024];
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                              reinterpret_cast<LPBYTE>(buffer), &size);
    ::RegCloseKey(key);

    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};

    const std::size_t length = std::min<std::size_t>(size / sizeof(wchar_t), std::size(buffer) - 1);
    buffer[length] = L'\0';

    if (type == REG_EXPAND_SZ) {
        wchar_t expanded[1024];
        const DWORD n = ::ExpandEnvironmentStringsW(buffer, expanded, static_cast<DWORD>(std::size(expanded)));
        if (n > 0 && n <= std::size(expanded)) return wideToUtf8(expanded);
    }

    return wideToUtf8(buffer);
}

void addIfUnseenAndPresent(std::vector<std::string>& out, const std::string& path) {
    if (path.empty()) return;
    const std::string normalised = pathNormalise(path);
    if (!directoryExists(normalised)) return;

    // Case-insensitive de-duplication: the registry and the conventional paths
    // very often name the same folder with different capitalisation.
    for (const std::string& existing : out) {
        if (existing.size() != normalised.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < existing.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(existing[i]))
                != std::tolower(static_cast<unsigned char>(normalised[i]))) { same = false; break; }
        }
        if (same) return;
    }
    out.push_back(normalised);
}

} // namespace

std::string applicationData() {
    std::string root = knownFolder(FOLDERID_RoamingAppData);
    if (root.empty()) {
        // Falling back to the executable's folder is better than failing to
        // start, even if it is not where settings belong.
        root = executableDirectory();
    }
    return pathJoin(root, "acomposter");
}

std::string settingsFile() { return pathJoin(applicationData(), "settings.json"); }
std::string pluginCacheFile() { return pathJoin(applicationData(), "plugin-cache.json"); }
std::string logFile() { return pathJoin(applicationData(), "acomposter.log"); }

std::string documents() {
    const std::string root = knownFolder(FOLDERID_Documents);
    return root.empty() ? applicationData() : root;
}

std::string patchesDirectory() { return pathJoin(documents(), "acomposter\\patches"); }
std::string recordingsDirectory() { return pathJoin(documents(), "acomposter\\recordings"); }

std::string desktop() { return knownFolder(FOLDERID_Desktop); }
std::string downloads() { return knownFolder(FOLDERID_Downloads); }
std::string musicFolder() { return knownFolder(FOLDERID_Music); }
std::string userProfile() { return knownFolder(FOLDERID_Profile); }

std::vector<std::string> driveRoots() {
    std::vector<std::string> roots;

    const DWORD mask = ::GetLogicalDrives();
    if (mask == 0) return roots;

    for (int letter = 0; letter < 26; ++letter) {
        if ((mask & (1u << letter)) == 0) continue;

        char path[4] = { static_cast<char>('A' + letter), ':', '\\', '\0' };

        // Skip anything with no medium in it: an empty optical drive would
        // otherwise sit in the sidebar and block for seconds when clicked.
        const UINT type = ::GetDriveTypeA(path);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE)
            continue;
        if (type == DRIVE_REMOVABLE && ::GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            continue;

        roots.emplace_back(path);
    }

    return roots;
}

std::string executableDirectory() {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0) return {};
    return pathParent(wideToUtf8(std::wstring_view(buffer, length)));
}

std::vector<std::string> defaultPluginDirectories() {
    std::vector<std::string> directories;

    // Where the VST2 installers were told to put things. On a 64-bit system the
    // 32-bit path lives under the WOW6432Node view, which is why both are read
    // explicitly rather than relying on redirection.
    addIfUnseenAndPresent(directories,
        registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VST", L"VSTPluginsPath", KEY_WOW64_64KEY));
    addIfUnseenAndPresent(directories,
        registryString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\VST", L"VSTPluginsPath", KEY_WOW64_32KEY));
    addIfUnseenAndPresent(directories,
        registryString(HKEY_CURRENT_USER, L"SOFTWARE\\VST", L"VSTPluginsPath", 0));

    // The conventional locations, which plenty of plugins use regardless of the
    // registry.
    const std::string programFiles = knownFolder(FOLDERID_ProgramFiles);
    const std::string programFilesX86 = knownFolder(FOLDERID_ProgramFilesX86);

    if (!programFiles.empty()) {
        addIfUnseenAndPresent(directories, pathJoin(programFiles, "VSTPlugins"));
        addIfUnseenAndPresent(directories, pathJoin(programFiles, "Steinberg\\VSTPlugins"));
        addIfUnseenAndPresent(directories, pathJoin(programFiles, "Common Files\\VST2"));
        addIfUnseenAndPresent(directories, pathJoin(programFiles, "Common Files\\Steinberg\\VST2"));
    }

    if (!programFilesX86.empty()) {
        addIfUnseenAndPresent(directories, pathJoin(programFilesX86, "VSTPlugins"));
        addIfUnseenAndPresent(directories, pathJoin(programFilesX86, "Steinberg\\VSTPlugins"));
        addIfUnseenAndPresent(directories, pathJoin(programFilesX86, "Common Files\\VST2"));
        addIfUnseenAndPresent(directories, pathJoin(programFilesX86, "Common Files\\Steinberg\\VST2"));
    }

    return directories;
}

void ensureDirectories() {
    createDirectories(applicationData());
    createDirectories(patchesDirectory());
    createDirectories(recordingsDirectory());
}

} // namespace acm::paths

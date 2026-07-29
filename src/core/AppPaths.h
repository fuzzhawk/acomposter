// Where acomposter keeps its own files.
//
// Settings, the plugin scan cache and the log live under the user's roaming
// application data; patches and recordings default to Documents. Nothing is
// written next to the executable, so the application still works when installed
// somewhere the user cannot write to.
#pragma once

#include <string>
#include <vector>

namespace acm::paths {

// %APPDATA%\acomposter - created on first use.
std::string applicationData();

std::string settingsFile();       // settings.json
std::string pluginCacheFile();    // plugin-cache.json
std::string logFile();            // acomposter.log

// Documents\acomposter - patches and rendered audio.
std::string documents();
std::string patchesDirectory();
std::string recordingsDirectory();

// The places worth putting one click away in the file browser. Samples live
// wherever the user keeps them, which is almost never under our own directories.
std::string desktop();
std::string downloads();
std::string musicFolder();
std::string userProfile();

// Roots of every fixed and removable volume that is currently mounted, as
// "C:\", "D:\" and so on. Without these the browser cannot reach a sample
// library on a second drive at all.
std::vector<std::string> driveRoots();

// Directory containing acomposter.exe, used to find the bridge helpers.
std::string executableDirectory();

// Plugin folders to scan by default: the paths Windows records for VST2, plus
// the conventional install locations for both architectures.
std::vector<std::string> defaultPluginDirectories();

// Makes sure the writable directories exist. Safe to call repeatedly.
void ensureDirectories();

} // namespace acm::paths

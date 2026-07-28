// Finds VST2 plugins, remembers what it found, and creates instances.
//
// Scanning is done out of process, in a bridge helper of the plugin's own
// architecture. That is not an optimisation: a plugin that crashes while being
// probed is common enough that scanning in-process would make the first run of
// the application a coin toss. Here, a bad plugin kills only the helper and gets
// recorded as broken.
//
// Results are cached on disk keyed by path, size and modification time, so the
// second start-up is instant and only genuinely new or changed plugins are
// probed again.
#pragma once

#include "Vst2Plugin.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acm::vst2 {

struct ScanProgress {
    bool running = false;
    int filesFound = 0;
    int filesScanned = 0;
    int pluginsFound = 0;
    int failures = 0;
    std::string currentFile;
};

// A plugin the scanner could not load, kept so the UI can explain itself rather
// than silently omitting something the user knows they installed.
struct FailedPlugin {
    std::string path;
    std::string reason;
    Architecture architecture = Architecture::Unknown;
};

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // -- search paths ------------------------------------------------------
    void setSearchPaths(std::vector<std::string> paths);
    const std::vector<std::string>& searchPaths() const noexcept { return searchPaths_; }
    void addSearchPath(std::string path);
    void removeSearchPath(const std::string& path);
    // Populates the search paths from the registry and the usual install spots.
    void useDefaultSearchPaths();

    // -- scanning ----------------------------------------------------------
    // Runs on a background thread. `rescanAll` ignores the cache.
    void startScan(bool rescanAll = false);
    void cancelScan();
    bool scanning() const noexcept { return scanning_.load(std::memory_order_relaxed); }
    ScanProgress progress() const;

    // -- results -----------------------------------------------------------
    std::vector<PluginDescription> plugins() const;
    std::vector<FailedPlugin> failures() const;
    const PluginDescription* findByIdentifier(const std::string& identifier) const;
    // Falls back to matching on the file name when the exact path has moved,
    // which is what usually happens when a patch is opened on another machine.
    const PluginDescription* findForPatch(const std::string& identifier,
                                          const std::string& path) const;

    // -- cache -------------------------------------------------------------
    bool loadCache(const std::string& utf8Path);
    bool saveCache(const std::string& utf8Path) const;

    // -- instantiation -----------------------------------------------------
    // `forceBridge` runs a native-architecture plugin out of process anyway,
    // trading a little latency for crash isolation.
    Vst2PluginPtr instantiate(const PluginDescription& description,
                              double sampleRate, int blockSize,
                              bool forceBridge, std::string* error) const;

    // Whether a plugin of this architecture would need the bridge.
    static bool requiresBridge(Architecture architecture) noexcept {
        return !isNativeArchitecture(architecture);
    }

private:
    void scanThreadMain(bool rescanAll);
    std::vector<std::string> collectCandidateFiles() const;

    mutable std::mutex mutex_;
    std::vector<std::string> searchPaths_;
    std::vector<PluginDescription> plugins_;
    std::vector<FailedPlugin> failures_;

    std::thread scanThread_;
    std::atomic<bool> scanning_{ false };
    std::atomic<bool> cancelRequested_{ false };

    mutable std::mutex progressMutex_;
    ScanProgress progress_;
};

} // namespace acm::vst2

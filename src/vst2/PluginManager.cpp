#include "PluginManager.h"

#include "../core/AppPaths.h"
#include "../core/FileIo.h"
#include "../core/Json.h"
#include "BridgedVst2Plugin.h"
#include "NativeVst2Plugin.h"
#include "PeArchitecture.h"

#include <algorithm>
#include <cctype>

namespace acm::vst2 {
namespace {

// Deep enough for the folder trees vendors actually create, shallow enough that
// a search path accidentally pointed at C:\ does not take all afternoon.
constexpr int kMaxScanDepth = 6;

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

void collectRecursively(const std::string& directory, int depth, std::vector<std::string>& out) {
    if (depth > kMaxScanDepth) return;

    for (const DirectoryEntry& entry : listDirectory(directory)) {
        if (entry.isDirectory) {
            collectRecursively(entry.fullPath, depth + 1, out);
            continue;
        }
        if (pathExtension(entry.name) == ".dll")
            out.push_back(entry.fullPath);
    }
}

} // namespace

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    cancelScan();
    if (scanThread_.joinable()) scanThread_.join();
}

// ---------------------------------------------------------------------------
// Search paths
// ---------------------------------------------------------------------------

void PluginManager::setSearchPaths(std::vector<std::string> paths) {
    std::lock_guard<std::mutex> lock(mutex_);
    searchPaths_ = std::move(paths);
}

void PluginManager::addSearchPath(std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string normalised = pathNormalise(path);
    for (const std::string& existing : searchPaths_)
        if (equalsIgnoreCase(existing, normalised)) return;
    searchPaths_.push_back(normalised);
}

void PluginManager::removeSearchPath(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    searchPaths_.erase(std::remove_if(searchPaths_.begin(), searchPaths_.end(),
                                      [&](const std::string& p) { return equalsIgnoreCase(p, path); }),
                       searchPaths_.end());
}

void PluginManager::useDefaultSearchPaths() {
    setSearchPaths(paths::defaultPluginDirectories());
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

std::vector<std::string> PluginManager::collectCandidateFiles() const {
    std::vector<std::string> roots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        roots = searchPaths_;
    }

    std::vector<std::string> files;
    for (const std::string& root : roots) {
        if (directoryExists(root)) collectRecursively(root, 0, files);
    }

    // The same plugin often appears through two overlapping search paths.
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    return files;
}

void PluginManager::startScan(bool rescanAll) {
    if (scanning_.load(std::memory_order_relaxed)) return;

    if (scanThread_.joinable()) scanThread_.join();

    cancelRequested_.store(false, std::memory_order_relaxed);
    scanning_.store(true, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress_ = ScanProgress{};
        progress_.running = true;
    }

    scanThread_ = std::thread(&PluginManager::scanThreadMain, this, rescanAll);
}

void PluginManager::cancelScan() {
    cancelRequested_.store(true, std::memory_order_relaxed);
}

void PluginManager::scanThreadMain(bool rescanAll) {
    const std::vector<std::string> files = collectCandidateFiles();

    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress_.filesFound = static_cast<int>(files.size());
    }

    // Everything already known, so an incremental scan can skip unchanged files.
    std::vector<PluginDescription> known;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        known = plugins_;
        if (rescanAll) {
            plugins_.clear();
            failures_.clear();
        }
    }

    std::vector<PluginDescription> found;
    std::vector<FailedPlugin> failed;

    for (const std::string& file : files) {
        if (cancelRequested_.load(std::memory_order_relaxed)) break;

        {
            std::lock_guard<std::mutex> lock(progressMutex_);
            progress_.currentFile = pathLeaf(file);
            ++progress_.filesScanned;
        }

        const std::int64_t size = fileSize(file);
        const std::int64_t modified = fileModifiedTime(file);

        if (!rescanAll) {
            const auto cached = std::find_if(known.begin(), known.end(),
                [&](const PluginDescription& d) {
                    return equalsIgnoreCase(d.path, file)
                        && d.fileSize == size && d.fileModifiedTime == modified;
                });
            if (cached != known.end()) {
                found.push_back(*cached);
                std::lock_guard<std::mutex> lock(progressMutex_);
                ++progress_.pluginsFound;
                continue;
            }
        }

        // Reading the PE header first means a folder full of ordinary DLLs is
        // rejected for a few hundred bytes each, without launching anything.
        std::string archError;
        const Architecture architecture = readPeArchitecture(file, &archError);
        if (architecture == Architecture::Unknown) continue;
        if (!isDynamicLibrary(file)) continue;

        PluginDescription description;
        std::string error;

        // Always out of process, even for a native-architecture plugin: this is
        // the step most likely to crash, and it must not take the app with it.
        if (!BridgedVst2Plugin::describe(file, architecture, description, &error)) {
            failed.push_back(FailedPlugin{ file, error.empty() ? "the plugin could not be loaded" : error,
                                           architecture });
            std::lock_guard<std::mutex> lock(progressMutex_);
            ++progress_.failures;
            continue;
        }

        description.path = file;
        description.architecture = architecture;
        description.fileSize = size;
        description.fileModifiedTime = modified;
        found.push_back(std::move(description));

        std::lock_guard<std::mutex> lock(progressMutex_);
        ++progress_.pluginsFound;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        plugins_ = std::move(found);
        failures_ = std::move(failed);

        std::sort(plugins_.begin(), plugins_.end(),
                  [](const PluginDescription& a, const PluginDescription& b) {
                      return std::lexicographical_compare(
                          a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                          [](char x, char y) {
                              return std::tolower(static_cast<unsigned char>(x))
                                   < std::tolower(static_cast<unsigned char>(y));
                          });
                  });
    }

    {
        std::lock_guard<std::mutex> lock(progressMutex_);
        progress_.running = false;
        progress_.currentFile.clear();
    }
    scanning_.store(false, std::memory_order_relaxed);
}

ScanProgress PluginManager::progress() const {
    std::lock_guard<std::mutex> lock(progressMutex_);
    return progress_;
}

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

std::vector<PluginDescription> PluginManager::plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return plugins_;
}

std::vector<FailedPlugin> PluginManager::failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failures_;
}

const PluginDescription* PluginManager::findByIdentifier(const std::string& identifier) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const PluginDescription& description : plugins_)
        if (description.identifier() == identifier) return &description;
    return nullptr;
}

const PluginDescription* PluginManager::findForPatch(const std::string& identifier,
                                                     const std::string& path) const {
    if (const PluginDescription* exact = findByIdentifier(identifier)) return exact;

    std::lock_guard<std::mutex> lock(mutex_);

    // The plugin has moved, or the patch came from another machine. Match on the
    // file name, which is stable far more often than the full path.
    const std::string leaf = pathLeaf(path);
    for (const PluginDescription& description : plugins_)
        if (equalsIgnoreCase(pathLeaf(description.path), leaf)) return &description;

    return nullptr;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

bool PluginManager::loadCache(const std::string& utf8Path) {
    std::string text;
    if (!readFileText(utf8Path, text)) return false;

    std::string parseError;
    const JsonValue root = JsonValue::parse(text, &parseError);
    if (!parseError.empty() || !root.isObject()) return false;

    std::vector<PluginDescription> loaded;
    if (const JsonValue* array = root.find("plugins")) {
        for (const JsonValue& entry : array->items()) {
            PluginDescription description = pluginDescriptionFromJson(entry);
            // A cached entry for a plugin that has since been uninstalled would
            // only produce a confusing failure later.
            if (description.valid() && fileExists(description.path))
                loaded.push_back(std::move(description));
        }
    }

    std::vector<FailedPlugin> loadedFailures;
    if (const JsonValue* array = root.find("failures")) {
        for (const JsonValue& entry : array->items()) {
            FailedPlugin failure;
            failure.path = entry.getString("path");
            failure.reason = entry.getString("reason");
            const int architecture = entry.getInt("architecture", 0);
            failure.architecture = (architecture == 1) ? Architecture::X86
                                 : (architecture == 2) ? Architecture::X64
                                                       : Architecture::Unknown;
            if (!failure.path.empty()) loadedFailures.push_back(std::move(failure));
        }
    }

    std::vector<std::string> loadedPaths;
    if (const JsonValue* array = root.find("searchPaths")) {
        for (const JsonValue& entry : array->items()) {
            const std::string path = entry.asString();
            if (!path.empty()) loadedPaths.push_back(path);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    plugins_ = std::move(loaded);
    failures_ = std::move(loadedFailures);
    if (!loadedPaths.empty()) searchPaths_ = std::move(loadedPaths);
    return true;
}

bool PluginManager::saveCache(const std::string& utf8Path) const {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-plugin-cache");
    root.set("version", 1);

    std::lock_guard<std::mutex> lock(mutex_);

    JsonValue searchPaths = JsonValue::array();
    for (const std::string& path : searchPaths_) searchPaths.push(JsonValue(path));
    root.set("searchPaths", searchPaths);

    JsonValue array = JsonValue::array();
    for (const PluginDescription& description : plugins_) array.push(toJson(description));
    root.set("plugins", array);

    JsonValue failureArray = JsonValue::array();
    for (const FailedPlugin& failure : failures_) {
        JsonValue entry = JsonValue::object();
        entry.set("path", failure.path);
        entry.set("reason", failure.reason);
        entry.set("architecture", static_cast<int>(failure.architecture));
        failureArray.push(entry);
    }
    root.set("failures", failureArray);

    return writeFileText(utf8Path, root.dump(2));
}

// ---------------------------------------------------------------------------
// Instantiation
// ---------------------------------------------------------------------------

Vst2PluginPtr PluginManager::instantiate(const PluginDescription& description,
                                         double sampleRate, int blockSize,
                                         bool forceBridge, std::string* error) const {
    if (!description.valid()) {
        if (error) *error = "the plugin description is incomplete";
        return nullptr;
    }
    if (!fileExists(description.path)) {
        if (error) *error = "the plugin file is no longer at " + description.path;
        return nullptr;
    }

    if (isNativeArchitecture(description.architecture) && !forceBridge) {
        auto plugin = std::make_unique<NativeVst2Plugin>();
        if (plugin->load(description.path, sampleRate, blockSize))
            return plugin;

        if (error) *error = plugin->errorText();
        // A native load that failed is still worth retrying through the bridge:
        // some plugins refuse to share an address space with anything else.
    }

    auto bridged = std::make_unique<BridgedVst2Plugin>();
    if (bridged->load(description.path, description.architecture, sampleRate, blockSize))
        return bridged;

    if (error) {
        const std::string bridgeError = bridged->errorText();
        if (!bridgeError.empty()) *error = bridgeError;
        else if (error->empty()) *error = "the plugin could not be loaded";
    }
    return nullptr;
}

} // namespace acm::vst2

// The librarian's index: every audio file under a folder, analysed once.
//
// Analysis is the expensive part - reading and transforming a few thousand
// files takes minutes - so it happens once, on a background thread, and the
// result is cached beside the library. A rescan only re-analyses files whose
// size or modification time changed, which makes reopening a folder of ten
// thousand samples instant rather than a coffee break.
//
// The index owns no audio. It records paths and numbers, and every operation
// that would change a file writes a new one instead.
#pragma once

#include "AudioAnalysis.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acm::library {

struct IndexedFile {
    std::string path;
    std::string name;         // leaf, for display and for the numbers
    std::int64_t sizeBytes = 0;
    std::int64_t modifiedTime = 0;

    Analysis analysis;
    std::string tagId;        // from the library's palette, empty when untagged
};

struct ScanProgress {
    bool running = false;
    int found = 0;
    int analysed = 0;
    int reused = 0;           // came back from the cache unchanged
    std::string currentFile;
};

// How the list is ordered. Sorting by something the analysis found is most of
// what the index is for: "show me the shortest", "show me the brightest".
enum class SortKey : int {
    Name = 0, Duration, Brightness, Pitch, Level, Number, Similarity, Count
};

const char* toString(SortKey key) noexcept;

struct Filter {
    std::string text;             // matches the name, case insensitive
    std::string tagId;            // empty matches every tag

    // Harmonic search: only files whose detected pitch is in this key, ignoring
    // octave. -128 means no key filter.
    int semitonesFromA4 = -128;

    // Seconds. A zero maximum means no upper bound.
    double minSeconds = 0.0;
    double maxSeconds = 0.0;

    // Only files confident enough to have a note name at all.
    bool pitchedOnly = false;
};

class FileIndex {
public:
    ~FileIndex();

    // Starts a scan on a background thread. Returns immediately; poll with
    // progress(). Calling it again cancels the previous scan first.
    void scan(std::string utf8Root, std::string cachePath);
    void cancelScan();
    ScanProgress progress() const;
    bool scanning() const noexcept { return scanning_.load(std::memory_order_acquire); }

    // Call once per UI frame from the message thread: publishes a finished
    // scan's results and writes the cache. Returns true on the frame a scan's
    // results first appear.
    bool serviceFromMessageThread();

    const std::string& root() const noexcept { return root_; }
    const std::vector<IndexedFile>& files() const noexcept { return files_; }

    const IndexedFile* find(const std::string& path) const;

    // The subset matching `filter`, ordered by `key`. When `key` is Similarity,
    // `reference` decides what "similar" means; otherwise it is ignored.
    std::vector<const IndexedFile*> query(const Filter& filter, SortKey key, bool descending,
                                          const std::string& reference = {}) const;

    // Applies a tag to one file's index entry. The library is the store of
    // record; this keeps the index in step without a rescan.
    void setTag(const std::string& path, std::string tagId);

    bool loadCache(const std::string& utf8Path);
    bool saveCache(const std::string& utf8Path) const;

private:
    void scanThreadMain(std::string root);

    std::string root_;
    std::string cachePath_;
    std::vector<IndexedFile> files_;

    // Written by the scan thread, swapped into files_ on the message thread.
    std::vector<IndexedFile> pending_;
    std::atomic<bool> pendingReady_{ false };

    std::thread thread_;
    std::atomic<bool> scanning_{ false };
    std::atomic<bool> cancelRequested_{ false };

    mutable std::mutex progressMutex_;
    ScanProgress progress_;
};

} // namespace acm::library

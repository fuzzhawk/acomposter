#include "FileIndex.h"

#include "../audio/AudioFile.h"
#include "../core/FileIo.h"
#include "../core/Json.h"

#include <algorithm>
#include <cctype>

namespace acm::library {
namespace {

const std::vector<std::string>& audioExtensions() {
    static const std::vector<std::string> extensions = { ".wav", ".aif", ".aiff" };
    return extensions;
}

bool containsNoCase(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;

    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(haystack[i + k]))
                != std::tolower(static_cast<unsigned char>(needle[k]))) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

// Depth-first, so a folder of folders comes back as one flat list. Bounded
// because a symlink loop or a mounted drive at the root would otherwise walk
// forever, and the librarian is pointed at sample folders rather than at C:\.
void collectFiles(const std::string& root, int depth, std::vector<DirectoryEntry>& out) {
    if (depth > 8) return;

    for (const DirectoryEntry& entry : listDirectory(root)) {
        if (entry.isDirectory) collectFiles(entry.fullPath, depth + 1, out);
        else {
            const std::string ext = pathExtension(entry.name);
            const auto& allowed = audioExtensions();
            if (std::find(allowed.begin(), allowed.end(), ext) != allowed.end())
                out.push_back(entry);
        }
    }
}

JsonValue analysisToJson(const Analysis& analysis) {
    JsonValue out = JsonValue::object();
    out.set("duration", analysis.durationSeconds);
    out.set("sampleRate", analysis.sampleRate);
    out.set("channels", analysis.channels);
    out.set("peak", analysis.peak);
    out.set("rms", analysis.rms);
    out.set("centroid", analysis.centroidHz);
    out.set("pitch", analysis.pitchHz);
    out.set("confidence", analysis.pitchConfidence);
    out.set("semitones", analysis.semitonesFromA4);
    out.set("note", analysis.noteName);
    out.set("attack", analysis.attackSeconds);
    out.set("decay", analysis.decaySeconds);
    out.set("bpm", analysis.bpm);

    JsonValue bands = JsonValue::array();
    for (const float band : analysis.bands) bands.push(band);
    out.set("bands", std::move(bands));

    JsonValue numbers = JsonValue::array();
    for (const int number : analysis.filenameNumbers) numbers.push(number);
    out.set("numbers", std::move(numbers));

    return out;
}

Analysis analysisFromJson(const JsonValue& in) {
    Analysis analysis;
    if (!in.isObject()) return analysis;

    analysis.valid = true;
    analysis.durationSeconds = in.getDouble("duration", 0.0);
    analysis.sampleRate = in.getDouble("sampleRate", 0.0);
    analysis.channels = in.getInt("channels", 0);
    analysis.peak = in.getFloat("peak", 0.0f);
    analysis.rms = in.getFloat("rms", 0.0f);
    analysis.centroidHz = in.getDouble("centroid", 0.0);
    analysis.pitchHz = in.getDouble("pitch", 0.0);
    analysis.pitchConfidence = in.getFloat("confidence", 0.0f);
    analysis.semitonesFromA4 = in.getInt("semitones", 0);
    analysis.noteName = in.getString("note");
    analysis.attackSeconds = in.getDouble("attack", 0.0);
    analysis.decaySeconds = in.getDouble("decay", 0.0);
    analysis.bpm = in.getDouble("bpm", 0.0);

    if (const JsonValue* bands = in.find("bands")) {
        for (std::size_t i = 0; i < bands->size() && i < kSpectrumBands; ++i)
            analysis.bands[i] = bands->at(i).asFloat(0.0f);
    }
    if (const JsonValue* numbers = in.find("numbers")) {
        for (std::size_t i = 0; i < numbers->size(); ++i)
            analysis.filenameNumbers.push_back(numbers->at(i).asInt(0));
    }

    return analysis;
}

} // namespace

const char* toString(SortKey key) noexcept {
    switch (key) {
        case SortKey::Duration:   return "length";
        case SortKey::Brightness: return "bright";
        case SortKey::Pitch:      return "pitch";
        case SortKey::Level:      return "level";
        case SortKey::Number:     return "number";
        case SortKey::Similarity: return "similar";
        case SortKey::Name:
        default:                  return "name";
    }
}

FileIndex::~FileIndex() {
    cancelScan();
    if (thread_.joinable()) thread_.join();
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void FileIndex::scan(std::string utf8Root, std::string cachePath) {
    cancelScan();
    if (thread_.joinable()) thread_.join();

    root_ = std::move(utf8Root);
    cachePath_ = std::move(cachePath);
    if (root_.empty()) return;

    cancelRequested_.store(false, std::memory_order_release);
    scanning_.store(true, std::memory_order_release);
    pendingReady_.store(false, std::memory_order_release);

    {
        const std::lock_guard<std::mutex> lock(progressMutex_);
        progress_ = ScanProgress{};
        progress_.running = true;
    }

    thread_ = std::thread([this, root = root_] { scanThreadMain(root); });
}

void FileIndex::cancelScan() {
    cancelRequested_.store(true, std::memory_order_release);
}

ScanProgress FileIndex::progress() const {
    const std::lock_guard<std::mutex> lock(progressMutex_);
    return progress_;
}

void FileIndex::scanThreadMain(std::string root) {
    std::vector<DirectoryEntry> found;
    collectFiles(root, 0, found);

    {
        const std::lock_guard<std::mutex> lock(progressMutex_);
        progress_.found = static_cast<int>(found.size());
    }

    std::vector<IndexedFile> result;
    result.reserve(found.size());

    for (const DirectoryEntry& entry : found) {
        if (cancelRequested_.load(std::memory_order_acquire)) break;

        {
            const std::lock_guard<std::mutex> lock(progressMutex_);
            progress_.currentFile = entry.name;
        }

        IndexedFile file;
        file.path = entry.fullPath;
        file.name = entry.name;
        file.sizeBytes = entry.size;
        file.modifiedTime = entry.modifiedTime;

        // Unchanged since the last scan means the analysis is still true. This
        // is what makes reopening a folder of ten thousand samples instant
        // rather than a two-minute wait for an answer already known.
        const IndexedFile* cached = find(entry.fullPath);
        if (cached && cached->sizeBytes == entry.size
            && cached->modifiedTime == entry.modifiedTime && cached->analysis.valid) {
            file.analysis = cached->analysis;
            file.tagId = cached->tagId;

            const std::lock_guard<std::mutex> lock(progressMutex_);
            ++progress_.reused;
        } else {
            std::string error;
            if (const auto buffer = audiofile::load(entry.fullPath, &error))
                file.analysis = analyse(*buffer, entry.name);

            const std::lock_guard<std::mutex> lock(progressMutex_);
            ++progress_.analysed;
        }

        result.push_back(std::move(file));
    }

    pending_ = std::move(result);
    pendingReady_.store(true, std::memory_order_release);
    scanning_.store(false, std::memory_order_release);

    const std::lock_guard<std::mutex> lock(progressMutex_);
    progress_.running = false;
    progress_.currentFile.clear();
}

bool FileIndex::serviceFromMessageThread() {
    if (!pendingReady_.load(std::memory_order_acquire)) return false;

    // The thread is finished with `pending_` by the time the flag is set, and
    // this is the only other reader, so the swap needs no lock.
    files_ = std::move(pending_);
    pending_.clear();
    pendingReady_.store(false, std::memory_order_release);

    if (thread_.joinable()) thread_.join();
    if (!cachePath_.empty()) saveCache(cachePath_);
    return true;
}

// ---------------------------------------------------------------------------
// Querying
// ---------------------------------------------------------------------------

const IndexedFile* FileIndex::find(const std::string& path) const {
    for (const IndexedFile& file : files_)
        if (file.path == path) return &file;
    return nullptr;
}

void FileIndex::setTag(const std::string& path, std::string tagId) {
    for (IndexedFile& file : files_) {
        if (file.path != path) continue;
        file.tagId = std::move(tagId);
        return;
    }
}

std::vector<const IndexedFile*> FileIndex::query(const Filter& filter, SortKey key,
                                                 bool descending,
                                                 const std::string& reference) const {
    std::vector<const IndexedFile*> out;

    for (const IndexedFile& file : files_) {
        if (!containsNoCase(file.name, filter.text)) continue;
        if (!filter.tagId.empty() && file.tagId != filter.tagId) continue;

        if (filter.pitchedOnly && file.analysis.noteName.empty()) continue;

        if (filter.semitonesFromA4 != -128) {
            if (file.analysis.noteName.empty()) continue;
            if (!sameePitchClass(file.analysis.semitonesFromA4, filter.semitonesFromA4)) continue;
        }

        if (file.analysis.durationSeconds < filter.minSeconds) continue;
        if (filter.maxSeconds > 0.0 && file.analysis.durationSeconds > filter.maxSeconds) continue;

        out.push_back(&file);
    }

    const IndexedFile* referenceFile = reference.empty() ? nullptr : find(reference);

    const auto valueOf = [&](const IndexedFile* file) -> double {
        switch (key) {
            case SortKey::Duration:   return file->analysis.durationSeconds;
            case SortKey::Brightness: return file->analysis.centroidHz;
            case SortKey::Pitch:      return file->analysis.noteName.empty()
                                             ? -1000.0
                                             : static_cast<double>(file->analysis.semitonesFromA4);
            case SortKey::Level:      return static_cast<double>(file->analysis.rms);
            case SortKey::Number:     return file->analysis.filenameNumbers.empty()
                                             ? -1.0
                                             : static_cast<double>(file->analysis.filenameNumbers.front());
            case SortKey::Similarity: return referenceFile
                                             ? static_cast<double>(similarity(file->analysis,
                                                                              referenceFile->analysis))
                                             : 0.0;
            default:                  return 0.0;
        }
    };

    if (key == SortKey::Name) {
        std::sort(out.begin(), out.end(), [descending](const IndexedFile* a, const IndexedFile* b) {
            return descending ? b->name < a->name : a->name < b->name;
        });
    } else {
        std::sort(out.begin(), out.end(), [&](const IndexedFile* a, const IndexedFile* b) {
            const double va = valueOf(a);
            const double vb = valueOf(b);
            // Ties fall back to the name, so a re-query does not shuffle the
            // rows a folder of identically-analysed files came back in.
            if (va == vb) return a->name < b->name;
            return descending ? vb < va : va < vb;
        });
    }

    return out;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

bool FileIndex::loadCache(const std::string& utf8Path) {
    std::string text;
    if (!readFileText(utf8Path, text)) return false;

    std::string error;
    const JsonValue root = JsonValue::parse(text, &error);
    if (!error.empty() || !root.isObject()) return false;

    files_.clear();

    const JsonValue* array = root.find("files");
    if (!array || !array->isArray()) return false;

    for (std::size_t i = 0; i < array->size(); ++i) {
        const JsonValue& entry = array->at(i);
        if (!entry.isObject()) continue;

        IndexedFile file;
        file.path = entry.getString("path");
        if (file.path.empty()) continue;

        file.name = entry.getString("name", pathLeaf(file.path));
        file.sizeBytes = entry.getInt64("size", 0);
        file.modifiedTime = entry.getInt64("modified", 0);
        file.tagId = entry.getString("tag");

        if (const JsonValue* analysis = entry.find("analysis"))
            file.analysis = analysisFromJson(*analysis);

        files_.push_back(std::move(file));
    }

    return true;
}

bool FileIndex::saveCache(const std::string& utf8Path) const {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-file-index");
    root.set("version", 1);
    root.set("root", root_);

    JsonValue array = JsonValue::array();
    for (const IndexedFile& file : files_) {
        JsonValue entry = JsonValue::object();
        entry.set("path", file.path);
        entry.set("name", file.name);
        entry.set("size", file.sizeBytes);
        entry.set("modified", file.modifiedTime);
        if (!file.tagId.empty()) entry.set("tag", file.tagId);
        if (file.analysis.valid) entry.set("analysis", analysisToJson(file.analysis));
        array.push(std::move(entry));
    }

    root.set("files", std::move(array));
    return writeFileText(utf8Path, root.dump(1));
}

} // namespace acm::library

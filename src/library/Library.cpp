#include "Library.h"

#include "../core/FileIo.h"

#include <algorithm>
#include <cctype>

namespace acm::library {
namespace {

std::string slugify(const std::string& text) {
    std::string out;
    for (char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) out.push_back(static_cast<char>(std::tolower(u)));
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("entry") : out;
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

constexpr const char* kEntryExtension = ".json";

} // namespace

const char* toString(EntryKind kind) noexcept {
    switch (kind) {
        case EntryKind::Project: return "project";
        case EntryKind::Asset:   return "asset";
        case EntryKind::Song:
        default:                 return "song";
    }
}

EntryKind entryKindFromString(const std::string& text) noexcept {
    if (text == "project") return EntryKind::Project;
    if (text == "asset") return EntryKind::Asset;
    return EntryKind::Song;
}

bool Entry::hasFile(const std::string& path) const {
    return std::find(files.begin(), files.end(), path) != files.end();
}

bool Entry::hasTag(const std::string& tagId) const {
    return std::find(tags.begin(), tags.end(), tagId) != tags.end();
}

// ---------------------------------------------------------------------------

std::string Library::entryPath(const std::string& id) const {
    return pathJoin(pathJoin(root_, "entries"), id + kEntryExtension);
}

std::string Library::makeUniqueId(const std::string& from) const {
    const std::string base = slugify(from);
    if (!find(base)) return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const std::string candidate = base + "-" + std::to_string(suffix);
        if (!find(candidate)) return candidate;
    }
    return base + "-x";
}

bool Library::open(const std::string& utf8RootDirectory, int* outSkipped) {
    if (outSkipped) *outSkipped = 0;
    if (utf8RootDirectory.empty()) return false;

    root_ = utf8RootDirectory;
    entries_.clear();

    const std::string entriesDirectory = pathJoin(root_, "entries");
    createDirectories(entriesDirectory);

    // The palette lives beside the entries so a library is one directory that
    // can be copied whole.
    const std::string palettePath = pathJoin(root_, "tags.json");
    if (!palette_.load(palettePath)) {
        palette_.loadDefaults();
        palette_.save(palettePath);
    }

    for (const DirectoryEntry& file : listDirectory(entriesDirectory, { kEntryExtension })) {
        if (file.isDirectory) continue;

        std::string text;
        if (!readFileText(file.fullPath, text)) {
            if (outSkipped) ++*outSkipped;
            continue;
        }

        std::string error;
        const JsonValue root = JsonValue::parse(text, &error);
        if (!error.empty() || !root.isObject()) {
            // One unreadable file costs one entry, never the library. It is
            // left on disk untouched so it can be repaired by hand.
            if (outSkipped) ++*outSkipped;
            continue;
        }

        Entry entry = fromJson(root);
        if (entry.id.empty()) entry.id = pathStem(file.name);
        if (!entry.id.empty()) entries_.push_back(std::move(entry));
    }

    return true;
}

std::vector<const Entry*> Library::entriesOfKind(EntryKind kind) const {
    std::vector<const Entry*> out;
    for (const Entry& entry : entries_)
        if (entry.kind == kind) out.push_back(&entry);

    std::sort(out.begin(), out.end(), [](const Entry* a, const Entry* b) {
        if (a->order != b->order) return a->order < b->order;
        return a->name < b->name;
    });
    return out;
}

const Entry* Library::find(const std::string& id) const {
    for (const Entry& entry : entries_)
        if (entry.id == id) return &entry;
    return nullptr;
}

Entry* Library::find(const std::string& id) {
    for (Entry& entry : entries_)
        if (entry.id == id) return &entry;
    return nullptr;
}

std::string Library::create(EntryKind kind, const std::string& name) {
    if (!isOpen()) return {};

    Entry entry;
    entry.id = makeUniqueId(name.empty() ? std::string(toString(kind)) : name);
    entry.kind = kind;
    entry.name = name.empty() ? entry.id : name;
    entry.order = static_cast<int>(entriesOfKind(kind).size());

    entries_.push_back(entry);
    save(entry.id);
    return entry.id;
}

bool Library::remove(const std::string& id) {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&](const Entry& e) { return e.id == id; });
    if (it == entries_.end()) return false;

    // The entry file goes; the audio it referenced does not. Nothing in the
    // library owns a file, so deleting an entry can never delete audio.
    deleteFile(entryPath(id));
    entries_.erase(it);

    for (Entry& entry : entries_) {
        entry.members.erase(std::remove(entry.members.begin(), entry.members.end(), id),
                            entry.members.end());
    }
    return true;
}

bool Library::save(const std::string& id) const {
    const Entry* entry = find(id);
    if (!entry || !isOpen()) return false;
    return writeFileText(entryPath(id), toJson(*entry).dump(2));
}

bool Library::saveAll() const {
    bool ok = true;
    for (const Entry& entry : entries_) ok = save(entry.id) && ok;
    return ok;
}

bool Library::addFile(const std::string& entryId, const std::string& path) {
    Entry* entry = find(entryId);
    if (!entry || path.empty()) return false;
    if (entry->hasFile(path)) return true;   // a drag arriving twice is normal

    entry->files.push_back(path);
    return save(entryId);
}

bool Library::removeFile(const std::string& entryId, const std::string& path) {
    Entry* entry = find(entryId);
    if (!entry) return false;

    const auto it = std::find(entry->files.begin(), entry->files.end(), path);
    if (it == entry->files.end()) return true;

    entry->files.erase(it);
    return save(entryId);
}

bool Library::addMember(const std::string& projectId, const std::string& songId) {
    Entry* project = find(projectId);
    if (!project || !find(songId)) return false;
    if (std::find(project->members.begin(), project->members.end(), songId)
        != project->members.end())
        return true;

    project->members.push_back(songId);
    return save(projectId);
}

bool Library::removeMember(const std::string& projectId, const std::string& songId) {
    Entry* project = find(projectId);
    if (!project) return false;

    const auto it = std::find(project->members.begin(), project->members.end(), songId);
    if (it == project->members.end()) return true;

    project->members.erase(it);
    return save(projectId);
}

std::vector<const Entry*> Library::entriesContaining(const std::string& path) const {
    std::vector<const Entry*> out;
    for (const Entry& entry : entries_)
        if (entry.hasFile(path)) out.push_back(&entry);
    return out;
}

bool Library::savePalette() const {
    if (!isOpen()) return false;
    return palette_.save(pathJoin(root_, "tags.json"));
}

std::string Library::tagForFile(const std::string& path) const {
    for (const Entry& entry : entries_) {
        if (entry.kind != EntryKind::Asset) continue;
        if (entry.hasFile(path) && !entry.tags.empty()) return entry.tags.front();
    }
    return {};
}

bool Library::setTagForFile(const std::string& path, const std::string& tagId) {
    if (path.empty()) return false;

    // One asset entry per file, found or made. Tagging a file is the smallest
    // thing the library does and must not require the user to have created
    // anything first.
    for (Entry& entry : entries_) {
        if (entry.kind != EntryKind::Asset || !entry.hasFile(path)) continue;

        entry.tags.clear();
        if (!tagId.empty()) entry.tags.push_back(tagId);
        return save(entry.id);
    }

    if (tagId.empty()) return true;   // nothing to record

    const std::string id = create(EntryKind::Asset, pathStem(path));
    Entry* entry = find(id);
    if (!entry) return false;

    entry->files.push_back(path);
    entry->tags.push_back(tagId);
    return save(id);
}

std::vector<const Entry*> Library::search(const std::string& text, EntryKind kind) const {
    std::vector<const Entry*> out;
    for (const Entry& entry : entries_) {
        if (entry.kind != kind) continue;
        if (text.empty()) { out.push_back(&entry); continue; }

        bool match = containsNoCase(entry.name, text) || containsNoCase(entry.notes, text);
        if (!match) {
            for (const std::string& file : entry.files)
                if (containsNoCase(pathLeaf(file), text)) { match = true; break; }
        }
        if (match) out.push_back(&entry);
    }
    return out;
}

std::vector<const Entry*> Library::withTag(const std::string& tagId) const {
    std::vector<const Entry*> out;
    for (const Entry& entry : entries_)
        if (entry.hasTag(tagId)) out.push_back(&entry);
    return out;
}

// ---------------------------------------------------------------------------

Entry Library::fromJson(const JsonValue& value) {
    Entry entry;
    entry.id = value.getString("id");
    entry.kind = entryKindFromString(value.getString("kind", "song"));
    entry.name = value.getString("name");
    entry.notes = value.getString("notes");
    entry.lyrics = value.getString("lyrics");
    entry.bpm = value.getDouble("bpm", 0.0);
    entry.key = value.getString("key");
    entry.order = value.getInt("order", 0);

    const auto readList = [&](const char* key, std::vector<std::string>& out) {
        const JsonValue* array = value.find(key);
        if (!array || !array->isArray()) return;
        for (std::size_t i = 0; i < array->size(); ++i) {
            const std::string text = array->at(i).asString();
            if (!text.empty()) out.push_back(text);
        }
    };

    readList("files", entry.files);
    readList("tags", entry.tags);
    readList("members", entry.members);

    if (entry.name.empty()) entry.name = entry.id;
    return entry;
}

JsonValue Library::toJson(const Entry& entry) {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-entry");
    root.set("version", 1);
    root.set("id", entry.id);
    root.set("kind", toString(entry.kind));
    root.set("name", entry.name);

    if (!entry.notes.empty()) root.set("notes", entry.notes);
    if (!entry.lyrics.empty()) root.set("lyrics", entry.lyrics);
    if (entry.bpm > 0.0) root.set("bpm", entry.bpm);
    if (!entry.key.empty()) root.set("key", entry.key);
    root.set("order", entry.order);

    const auto writeList = [&](const char* key, const std::vector<std::string>& from) {
        if (from.empty()) return;
        JsonValue array = JsonValue::array();
        for (const std::string& text : from) array.push(JsonValue(text));
        root.set(key, array);
    };

    writeList("files", entry.files);
    writeList("tags", entry.tags);
    writeList("members", entry.members);
    return root;
}

} // namespace acm::library

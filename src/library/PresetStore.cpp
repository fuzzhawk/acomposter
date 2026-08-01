#include "PresetStore.h"

#include "../core/FileIo.h"

#include <algorithm>

namespace acm::library {

void PresetStore::open(const std::string& utf8LibraryRoot, std::string subdirectory) {
    if (utf8LibraryRoot.empty()) { directory_.clear(); return; }

    directory_ = pathJoin(utf8LibraryRoot, subdirectory);
    createDirectories(directory_);
}

std::string PresetStore::pathFor(const std::string& name) const {
    return pathJoin(directory_, safeFileName(name, "preset") + ".json");
}

std::vector<std::string> PresetStore::names() const {
    std::vector<std::string> out;
    if (!isOpen()) return out;

    for (const DirectoryEntry& file : listDirectory(directory_, { ".json" })) {
        if (file.isDirectory) continue;

        std::string text;
        if (!readFileText(file.fullPath, text)) continue;

        std::string error;
        const JsonValue root = JsonValue::parse(text, &error);
        if (!error.empty() || !root.isObject()) continue;

        out.push_back(root.getString("name", pathStem(file.name)));
    }

    std::sort(out.begin(), out.end());
    return out;
}

bool PresetStore::load(const std::string& name, JsonValue& out) const {
    if (!isOpen()) return false;

    std::string text;
    if (!readFileText(pathFor(name), text)) return false;

    std::string error;
    out = JsonValue::parse(text, &error);
    return error.empty() && out.isObject();
}

bool PresetStore::save(const std::string& name, JsonValue document) const {
    if (!isOpen() || name.empty() || !document.isObject()) return false;

    document.set("name", name);
    return writeFileText(pathFor(name), document.dump(2));
}

bool PresetStore::saveIfAbsent(const std::string& name, JsonValue document) const {
    if (exists(name)) return false;
    return save(name, std::move(document));
}

bool PresetStore::remove(const std::string& name) {
    if (!isOpen()) return false;
    return deleteFile(pathFor(name));
}

bool PresetStore::exists(const std::string& name) const {
    return isOpen() && fileExists(pathFor(name));
}

} // namespace acm::library

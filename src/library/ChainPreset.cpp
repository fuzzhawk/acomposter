#include "ChainPreset.h"

#include "../core/Base64.h"
#include "../core/FileIo.h"

#include <algorithm>
#include <cctype>

namespace acm::library {
namespace {

// File names have to survive being typed by hand and copied between machines,
// so they are reduced rather than trusted.
std::string safeFileName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '-' || c == '_') out.push_back(c);
        else if (!out.empty() && out.back() != '-') out.push_back('-');
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? std::string("chain") : out;
}

} // namespace

void ChainStore::open(const std::string& utf8LibraryRoot) {
    if (utf8LibraryRoot.empty()) { directory_.clear(); return; }
    directory_ = pathJoin(utf8LibraryRoot, "chains");
    createDirectories(directory_);
}

std::string ChainStore::pathFor(const std::string& name) const {
    return pathJoin(directory_, safeFileName(name) + ".json");
}

std::vector<std::string> ChainStore::names() const {
    std::vector<std::string> out;
    if (!isOpen()) return out;

    for (const DirectoryEntry& file : listDirectory(directory_, { ".json" })) {
        if (file.isDirectory) continue;

        // The display name comes from inside the file rather than from the file
        // name, because the file name has been through safeFileName.
        std::string text;
        if (!readFileText(file.fullPath, text)) continue;

        std::string error;
        const JsonValue root = JsonValue::parse(text, &error);
        if (!error.empty() || !root.isObject()) continue;

        const std::string name = root.getString("name", pathStem(file.name));
        if (!name.empty()) out.push_back(name);
    }

    std::sort(out.begin(), out.end());
    return out;
}

bool ChainStore::load(const std::string& name, ChainPreset& out) const {
    if (!isOpen()) return false;

    std::string text;
    if (!readFileText(pathFor(name), text)) return false;

    std::string error;
    const JsonValue root = JsonValue::parse(text, &error);
    if (!error.empty() || !root.isObject()) return false;

    out = fromJson(root);
    return !out.plugins.empty();
}

bool ChainStore::save(const ChainPreset& preset) const {
    if (!isOpen() || preset.name.empty()) return false;
    return writeFileText(pathFor(preset.name), toJson(preset).dump(2));
}

bool ChainStore::remove(const std::string& name) {
    if (!isOpen()) return false;
    return deleteFile(pathFor(name));
}

JsonValue ChainStore::toJson(const ChainPreset& preset) {
    JsonValue root = JsonValue::object();
    root.set("format", "acomposter-chain");
    root.set("version", 1);
    root.set("name", preset.name);

    JsonValue array = JsonValue::array();
    for (const ChainPlugin& plugin : preset.plugins) {
        JsonValue entry = JsonValue::object();
        entry.set("name", plugin.name);
        entry.set("path", plugin.path);
        entry.set("uniqueId", static_cast<int>(plugin.uniqueId));
        entry.set("bridged", plugin.bridged);

        if (!plugin.state.empty())
            entry.set("state", base64Encode(plugin.state.data(), plugin.state.size()));

        if (!plugin.parameters.empty()) {
            JsonValue values = JsonValue::array();
            for (float value : plugin.parameters) values.push(JsonValue(value));
            entry.set("parameters", values);
        }

        array.push(entry);
    }
    root.set("plugins", array);
    return root;
}

ChainPreset ChainStore::fromJson(const JsonValue& value) {
    ChainPreset preset;
    preset.name = value.getString("name");

    const JsonValue* array = value.find("plugins");
    if (!array || !array->isArray()) return preset;

    for (std::size_t i = 0; i < array->size(); ++i) {
        const JsonValue& entry = array->at(i);

        ChainPlugin plugin;
        plugin.name = entry.getString("name");
        plugin.path = entry.getString("path");
        plugin.uniqueId = static_cast<std::int32_t>(entry.getInt("uniqueId", 0));
        plugin.bridged = entry.getBool("bridged", false);

        if (const std::string state = entry.getString("state"); !state.empty())
            plugin.state = base64Decode(state);

        if (const JsonValue* values = entry.find("parameters"); values && values->isArray()) {
            for (std::size_t p = 0; p < values->size(); ++p)
                plugin.parameters.push_back(static_cast<float>(values->at(p).asDouble(0.0)));
        }

        if (!plugin.path.empty() || plugin.uniqueId != 0)
            preset.plugins.push_back(std::move(plugin));
    }
    return preset;
}

} // namespace acm::library
